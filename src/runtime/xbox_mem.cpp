#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "xbox_mem.h"
#include "hle_common.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

uint8_t* g_xbox_mem_base = nullptr;
XbeImageInfo g_xbe_info;

namespace {
struct AllocBlock {
    uint32_t va;
    uint32_t size;
    bool used;
};

std::mutex g_alloc_mtx;
std::map<uint32_t, uint32_t> g_used_blocks; // va -> size
std::vector<AllocBlock> g_free_blocks;
uint32_t g_next_bump = HleKernelDataLayout::ARENA_ALLOC_BASE;
uint32_t g_next_teb_va = 0x00006000u; // 0x6000..0xFFFF reserved for secondary thread TEBs

uint32_t rd_u32_le(const uint8_t* p, size_t off) {
    uint32_t v;
    std::memcpy(&v, p + off, 4);
    return v;
}
} // namespace

bool xbox_mem_init() {
    if (g_xbox_mem_base) return true;
#ifdef _WIN32
    g_xbox_mem_base = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, XBOX_MEM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_xbox_mem_base) {
        std::fprintf(stderr, "[MEM] Failed to allocate 256 MiB guest arena (err=%lu)\n", GetLastError());
        return false;
    }
#else
    void* p = mmap(nullptr, XBOX_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        std::fprintf(stderr, "[MEM] Failed to mmap 256 MiB guest arena\n");
        return false;
    }
    g_xbox_mem_base = static_cast<uint8_t*>(p);
#endif
    std::memset(g_xbox_mem_base, 0, 0x00010000u);
    g_next_bump = HleKernelDataLayout::ARENA_ALLOC_BASE;
    g_next_teb_va = 0x00006000u;
    g_used_blocks.clear();
    g_free_blocks.clear();
    return true;
}

void xbox_mem_shutdown() {
    if (g_xbox_mem_base) {
#ifdef _WIN32
        VirtualFree(g_xbox_mem_base, 0, MEM_RELEASE);
#else
        munmap(g_xbox_mem_base, XBOX_MEM_SIZE);
#endif
        g_xbox_mem_base = nullptr;
    }
}

bool xbox_load_xbe_from_memory(const uint8_t* xbe, size_t size) {
    if (!g_xbox_mem_base && !xbox_mem_init()) return false;
    if (size < 0x200 || std::memcmp(xbe, "XBEH", 4) != 0) {
        std::fprintf(stderr, "[XBE] Invalid XBE magic\n");
        return false;
    }

    uint32_t base_addr    = rd_u32_le(xbe, 0x104);
    uint32_t headers_size = rd_u32_le(xbe, 0x108);
    uint32_t image_size   = rd_u32_le(xbe, 0x10C);
    uint32_t cert_va      = rd_u32_le(xbe, 0x118);
    uint32_t num_sections = rd_u32_le(xbe, 0x11C);
    uint32_t sec_hdr_va   = rd_u32_le(xbe, 0x120);
    uint32_t enc_entry    = rd_u32_le(xbe, 0x128);
    uint32_t tls_va       = rd_u32_le(xbe, 0x12C);
    uint32_t stack_commit = rd_u32_le(xbe, 0x134);
    uint32_t enc_thunk    = rd_u32_le(xbe, 0x158);

    g_xbe_info.base_addr = base_addr;
    g_xbe_info.headers_size = headers_size;
    g_xbe_info.image_size = image_size;
    g_xbe_info.entry_va = enc_entry ^ 0xA8FC57ABu;
    g_xbe_info.thunk_va = enc_thunk ^ 0x5B6D40B6u;
    g_xbe_info.tls_va = tls_va;
    g_xbe_info.stack_commit = stack_commit ? stack_commit : 0x00100000u;

    // Copy headers into base_addr
    size_t hdr_copy = std::min<size_t>(headers_size, size);
    std::memcpy(mem_ptr(base_addr), xbe, hdr_copy);

    if (cert_va >= base_addr && cert_va - base_addr + 12 <= size) {
        g_xbe_info.title_id = rd_u32_le(xbe, cert_va - base_addr + 8);
    }

    g_xbe_info.sections.clear();
    uint32_t sec_off = sec_hdr_va - base_addr;
    for (uint32_t i = 0; i < num_sections; ++i) {
        size_t o = sec_off + i * 56u;
        if (o + 56 > size) break;
        XbeSectionInfo si;
        si.flags   = rd_u32_le(xbe, o + 0);
        si.va      = rd_u32_le(xbe, o + 4);
        si.vsz     = rd_u32_le(xbe, o + 8);
        si.raw_off = rd_u32_le(xbe, o + 12);
        si.raw_sz  = rd_u32_le(xbe, o + 16);
        uint32_t name_va = rd_u32_le(xbe, o + 20);
        if (name_va >= base_addr && name_va - base_addr < size) {
            si.name = reinterpret_cast<const char*>(xbe + (name_va - base_addr));
        }
        if (si.va + si.vsz <= XBOX_MEM_SIZE) {
            std::memset(mem_ptr(si.va), 0, si.vsz);
            size_t copy_sz = std::min<size_t>(si.raw_sz, si.vsz);
            if (si.raw_off + copy_sz <= size) {
                std::memcpy(mem_ptr(si.va), xbe + si.raw_off, copy_sz);
            }
        }
        g_xbe_info.sections.push_back(si);
    }

    std::printf("[XBE] Loaded TitleID=0x%08X Entry=0x%08X Thunk=0x%08X Sections=%u\n",
                g_xbe_info.title_id, g_xbe_info.entry_va, g_xbe_info.thunk_va,
                static_cast<unsigned>(g_xbe_info.sections.size()));
    return true;
}

#ifdef _WIN32
static LONG CALLBACK xbox_ro_veh(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    ULONG_PTR rw = ep->ExceptionRecord->ExceptionInformation[0];
    ULONG_PTR addr = ep->ExceptionRecord->ExceptionInformation[1];
    uintptr_t base = reinterpret_cast<uintptr_t>(g_xbox_mem_base);
    if (g_xbox_mem_base && addr >= base && addr < base + XBOX_MEM_SIZE) {
        uint32_t gva = static_cast<uint32_t>(addr - base);
        std::fprintf(stderr, "\n[FAULT] Access violation (%s) at guest VA 0x%08X host RIP=0x%p\n",
                     rw == 1 ? "WRITE" : (rw == 0 ? "READ" : "EXEC"),
                     gva, reinterpret_cast<void*>(ep->ContextRecord->Rip));
        HANDLE proc = GetCurrentProcess();
        SymInitialize(proc, nullptr, TRUE);
        alignas(SYMBOL_INFO) uint8_t sym_buf[sizeof(SYMBOL_INFO) + 256]{};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, ep->ContextRecord->Rip, &disp, sym)) {
            IMAGEHLP_LINE64 line{sizeof(IMAGEHLP_LINE64)};
            DWORD ldisp = 0;
            if (SymGetLineFromAddr64(proc, ep->ContextRecord->Rip, &ldisp, &line)) {
                std::fprintf(stderr, "        Host func: %s + 0x%llX (%s:%lu)\n",
                             sym->Name, disp, line.FileName, line.LineNumber);
            } else {
                std::fprintf(stderr, "        Host func: %s + 0x%llX\n", sym->Name, disp);
            }
        }
        std::fflush(stderr);
        hle_kernel_dump_stall_state();
        TerminateProcess(GetCurrentProcess(), 0xC0000005u);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void xbox_ro_sigsegv(int sig, siginfo_t* si, void*) {
    (void)sig;
    uintptr_t addr = si ? reinterpret_cast<uintptr_t>(si->si_addr) : 0;
    uintptr_t base = reinterpret_cast<uintptr_t>(g_xbox_mem_base);
    if (g_xbox_mem_base && addr >= base && addr < base + XBOX_MEM_SIZE) {
        uint32_t gva = static_cast<uint32_t>(addr - base);
        std::fprintf(stderr, "\n[FAULT] SIGSEGV at guest VA 0x%08X\n", gva);
        std::fflush(stderr);
        hle_kernel_dump_stall_state();
    } else {
        std::fprintf(stderr, "\n[FAULT] SIGSEGV at host addr %p\n", si ? si->si_addr : nullptr);
    }
    _exit(139);
}
#endif

void xbox_mem_protect_readonly() {
    if (!g_xbox_mem_base || g_xbe_info.sections.empty()) return;
    static bool veh_added = false;
    if (!veh_added) {
        veh_added = true;
#ifdef _WIN32
        AddVectoredExceptionHandler(1, xbox_ro_veh);
#else
        struct sigaction sa{};
        sa.sa_sigaction = xbox_ro_sigsegv;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigaction(SIGSEGV, &sa, nullptr);
#endif
    }
    // Protect read-only XBE code sections (.text, XONLINE, XNET, XPP) strictly page-aligned inside spans
    // where flags bit 0 (WRITABLE) is 0 and no writable section overlaps the 4KB page.
    size_t protected_bytes = 0;
    uint32_t max_ro = 0;
    for (const auto& s : g_xbe_info.sections) {
        if (s.flags & 1u) { // first writable section stops contiguous RO span
            break;
        }
        if (s.va + s.vsz > max_ro) max_ro = s.va + s.vsz;
    }
    if (max_ro > g_xbe_info.base_addr) {
        uint32_t span_start = g_xbe_info.base_addr + 0x2000u; // 0x00012000 (.text start)
        uint32_t span_end = max_ro & ~0xFFFu;
        if (span_end > span_start) {
            size_t sz = span_end - span_start;
#ifdef _WIN32
            DWORD oldp = 0;
            if (VirtualProtect(mem_ptr(span_start), sz, PAGE_READONLY, &oldp)) {
                protected_bytes = sz;
            }
#else
            if (mprotect(mem_ptr(span_start), sz, PROT_READ) == 0) {
                protected_bytes = sz;
            }
#endif
        }
    }
    // Null-pointer guard page at VA 0x00000000..0x00000FFF (KPCR starts at 0x00001000)
#ifdef _WIN32
    DWORD oldp0 = 0;
    VirtualProtect(mem_ptr(0), 0x1000u, PAGE_NOACCESS, &oldp0);
#else
    mprotect(mem_ptr(0), 0x1000u, PROT_NONE);
#endif
    std::printf("[MEM] Write-protected read-only code span (0x%zX bytes) + Page 0 guard.\n", protected_bytes);
}

uint32_t xbox_alloc_teb(uint32_t stack_base_va, uint32_t stack_limit_va, uint32_t tls_size, uint32_t thread_id) {
    std::lock_guard<std::mutex> lk(g_alloc_mtx);
    uint32_t kpcr_va = g_next_teb_va;
    if (kpcr_va + 0x1000u <= 0x00010000u) {
        g_next_teb_va += 0x1000u;
    } else {
        // Fallback inside arena if many threads spawned
        kpcr_va = g_next_bump;
        g_next_bump += 0x1000u;
    }
    if (thread_id == 1) {
        kpcr_va = HleKernelDataLayout::KPCR_PRIMARY_VA;
    }
    std::memset(mem_ptr(kpcr_va), 0, 0x1000u);

    uint32_t prcb_va    = kpcr_va + 0x200u;
    uint32_t kthread_va = kpcr_va + 0x600u;
    uint32_t tls_arr_va = kpcr_va + 0xA00u;
    // Xbox CRT indexes negatively from fs:[4] (_tls_index = -5 -> fs:[4] - 0x14)
    uint32_t eff_tls = (tls_size >= 0x14u) ? tls_size : 0x14u;
    uint32_t tls_data_va = (eff_tls <= 0x400u) ? (kpcr_va + 0xB00u) : ((g_next_bump + 0xFFFu) & ~0xFFFu);
    if (tls_data_va >= HleKernelDataLayout::ARENA_ALLOC_BASE) {
        uint32_t aligned = (eff_tls + 0xFFFu) & ~0xFFFu;
        g_next_bump = tls_data_va + aligned;
    }
    std::memset(mem_ptr(tls_data_va), 0, eff_tls);
    wr32(tls_data_va, tls_data_va + 4u);
    uint32_t tls_end_va = tls_data_va + eff_tls;

    // Xbox KPCR layout observed from XBE (0x002D386C / 0x001DE054 / 0x001DD199):
    // +0x00: ExceptionList (-1)
    // +0x04: StackBase -> top of TLS data block; _tls_index = -5 reads *(fs:[4] - 0x14)
    // +0x08: StackLimit
    // +0x18: Self (kpcr_va)
    // +0x1C: SelfPcr (kpcr_va)
    // +0x20: Prcb (prcb_va)
    // +0x24: CurrentIrql (0)
    // +0x28: CurrentThread (kthread_va)
    wr32(kpcr_va + 0x00u, 0xFFFFFFFFu);
    wr32(kpcr_va + 0x04u, tls_end_va);
    wr32(kpcr_va + 0x08u, stack_limit_va);
    wr32(kpcr_va + 0x18u, kpcr_va);
    wr32(kpcr_va + 0x1Cu, kpcr_va);
    wr32(kpcr_va + 0x20u, prcb_va);
    wr8 (kpcr_va + 0x24u, 0);
    wr32(kpcr_va + 0x28u, kthread_va);

    // KPRCB +0x004: CurrentThread
    wr32(prcb_va + 0x004u, kthread_va);

    // KTHREAD fields observed:
    // +0x28: TlsData (non-zero checks in XAPI/XapiThreadStartup/SetLastError)
    // +0x12C: ThreadId / LastError slot
    wr32(kthread_va + 0x28u, tls_data_va);
    wr32(kthread_va + 0x12Cu, thread_id);
    wr32(tls_arr_va, tls_data_va);
    if (eff_tls <= 0x400u && (tls_end_va + 4u) <= kpcr_va + 0x1000u) {
        wr32(tls_end_va, tls_data_va + 4u);
    }

    // Preserve stack_base_va at +0x14 for diagnostics
    wr32(kpcr_va + 0x14u, stack_base_va);
    return kpcr_va;
}

uint32_t xbox_guest_alloc(uint32_t size, uint32_t align, bool zero_init) {
    if (size == 0) size = 16;
    if (align < 16) align = 16;
    uint32_t aligned_sz = (size + align - 1u) & ~(align - 1u);

    std::lock_guard<std::mutex> lk(g_alloc_mtx);
    // Try best/first fit from free list with matching alignment
    for (size_t i = 0; i < g_free_blocks.size(); ++i) {
        auto& b = g_free_blocks[i];
        if ((b.va & (align - 1u)) == 0 && b.size >= aligned_sz) {
            uint32_t va = b.va;
            uint32_t rem = b.size - aligned_sz;
            if (rem >= 4096u) {
                b.va += aligned_sz;
                b.size = rem;
            } else {
                aligned_sz = b.size;
                g_free_blocks.erase(g_free_blocks.begin() + static_cast<ptrdiff_t>(i));
            }
            g_used_blocks[va] = aligned_sz;
            if (zero_init) std::memset(mem_ptr(va), 0, aligned_sz);
            return va;
        }
    }

    uint32_t va = (g_next_bump + align - 1u) & ~(align - 1u);
    if (va + aligned_sz > HleKernelDataLayout::ARENA_ALLOC_END) {
        std::fprintf(stderr, "[MEM] Out of guest memory allocating 0x%X bytes!\n", aligned_sz);
        return 0;
    }
    g_next_bump = va + aligned_sz;
    g_used_blocks[va] = aligned_sz;
    if (zero_init) std::memset(mem_ptr(va), 0, aligned_sz);
    return va;
}

bool xbox_guest_free(uint32_t va) {
    if (va == 0) return true;
    std::lock_guard<std::mutex> lk(g_alloc_mtx);
    auto it = g_used_blocks.find(va);
    if (it == g_used_blocks.end()) return false;
    g_free_blocks.push_back({va, it->second, false});
    g_used_blocks.erase(it);
    return true;
}

uint32_t xbox_guest_alloc_size(uint32_t va) {
    std::lock_guard<std::mutex> lk(g_alloc_mtx);
    auto it = g_used_blocks.find(va);
    return it != g_used_blocks.end() ? it->second : 0;
}

void xbox_guest_query_stats(uint32_t* total_pages, uint32_t* avail_pages) {
    std::lock_guard<std::mutex> lk(g_alloc_mtx);
    // Report 64 MiB (16384 * 4KB pages) total retail or 128 MiB devkit cleanly
    constexpr uint32_t total = 32768u; // 128 MiB in 4KB pages
    uint64_t used_bytes = 0;
    for (auto& kv : g_used_blocks) used_bytes += kv.second;
    uint32_t used_pages = static_cast<uint32_t>((used_bytes + 4095u) >> 12);
    if (total_pages) *total_pages = total;
    if (avail_pages) *avail_pages = (total > used_pages) ? (total - used_pages) : 2048u;
}

std::string xbox_read_cstr(uint32_t va, size_t max_len) {
    if (!va || va >= XBOX_MEM_SIZE) return {};
    const char* p = reinterpret_cast<const char*>(mem_ptr(va));
    size_t limit = std::min<size_t>(max_len, XBOX_MEM_SIZE - va);
    size_t n = 0;
    while (n < limit && p[n] != '\0') ++n;
    return std::string(p, n);
}

std::string xbox_read_ansi_string(uint32_t pan_va) {
    if (!pan_va || pan_va + 8 > XBOX_MEM_SIZE) return {};
    uint16_t len = rd16(pan_va);
    uint32_t buf = rd32(pan_va + 4);
    if (!buf || len == 0 || buf + len > XBOX_MEM_SIZE) return {};
    return std::string(reinterpret_cast<const char*>(mem_ptr(buf)), len);
}

void xbox_write_cstr(uint32_t va, const char* s, size_t max_buf) {
    if (!va || max_buf == 0 || va + max_buf > XBOX_MEM_SIZE) return;
    size_t n = std::strlen(s);
    if (n >= max_buf) n = max_buf - 1;
    std::memcpy(mem_ptr(va), s, n);
    wr8(va + static_cast<uint32_t>(n), 0);
}
