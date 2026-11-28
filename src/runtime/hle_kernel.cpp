#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef INFINITE
#define INFINITE 0xFFFFFFFFu
#endif
#ifndef FILE_ATTRIBUTE_READONLY
#define FILE_ATTRIBUTE_READONLY  0x00000001u
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define FILE_ATTRIBUTE_NORMAL    0x00000080u
#endif
#ifndef ERROR_GEN_FAILURE
#define ERROR_GEN_FAILURE        31u
#define ERROR_FILE_NOT_FOUND     2u
#define ERROR_ALREADY_EXISTS     183u
#define ERROR_HANDLE_EOF         38u
#define ERROR_NO_MORE_FILES      18u
#endif
#endif
#include "hle_common.h"
#include "xiso_vfs.h"
#include "rl_achievements.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

bool g_hle_trace = false;

namespace {

constexpr uint32_t STATUS_SUCCESS_XB          = 0x00000000u;
constexpr uint32_t STATUS_PENDING_XB          = 0x00000103u;
constexpr uint32_t STATUS_NO_MORE_FILES_XB    = 0x80000006u;
constexpr uint32_t STATUS_INVALID_HANDLE_XB   = 0xC0000008u;
constexpr uint32_t STATUS_END_OF_FILE_XB      = 0xC0000011u;
constexpr uint32_t STATUS_NO_SUCH_FILE_XB     = 0xC000000Fu;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND_XB = 0xC0000034u;
constexpr uint32_t STATUS_OBJECT_NAME_COLLISION_XB = 0xC0000035u;
constexpr uint32_t STATUS_NOT_IMPLEMENTED_XB  = 0xC0000002u;

#ifdef _WIN32
using HostSync = HANDLE;
using HostFile = HANDLE;
const HostFile HOST_INVALID_FILE = INVALID_HANDLE_VALUE;
#else
struct HostSyncObj {
    enum class Kind { Event, Semaphore, Mutant };
    Kind kind = Kind::Event;
    bool manual_reset = true;
    bool signaled = false;
    int32_t count = 0;
    int32_t max_count = 1;
    std::thread::id owner{};
    int32_t depth = 0;
};
using HostSync = std::shared_ptr<HostSyncObj>;
using HostFile = int;
constexpr HostFile HOST_INVALID_FILE = -1;
std::mutex g_sync_mtx;
std::condition_variable g_sync_cv;
#endif

std::atomic<bool> g_krnl_shutting_down{false};
std::condition_variable g_thr_exit_cv;

inline void check_guest_shutdown() {
    if (g_krnl_shutting_down.load(std::memory_order_acquire)) {
        throw GuestPowerOff{};
    }
}

uint64_t host_ticks_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
}

void host_sleep_ms(uint32_t ms) {
    if (ms == 0) { std::this_thread::yield(); return; }
#ifdef _WIN32
    uint32_t rem = ms;
    while (rem > 0 && !g_krnl_shutting_down.load(std::memory_order_acquire)) {
        uint32_t step = std::min(rem, 50u);
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        rem -= step;
    }
#else
    std::unique_lock<std::mutex> lk(g_sync_mtx);
    g_sync_cv.wait_for(lk, std::chrono::milliseconds(ms), []() {
        return g_krnl_shutting_down.load(std::memory_order_acquire);
    });
#endif
}

int ci_strcmp(const char* a, const char* b) {
#ifdef _WIN32
    return _stricmp(a, b);
#else
    return ::strcasecmp(a, b);
#endif
}

int ci_strncmp(const char* a, const char* b, size_t n) {
#ifdef _WIN32
    return _strnicmp(a, b, n);
#else
    return ::strncasecmp(a, b, n);
#endif
}

HostSync host_create_event(bool manual_reset, bool initial_state) {
#ifdef _WIN32
    return CreateEventA(nullptr, manual_reset ? TRUE : FALSE, initial_state ? TRUE : FALSE, nullptr);
#else
    auto s = std::make_shared<HostSyncObj>();
    s->kind = HostSyncObj::Kind::Event;
    s->manual_reset = manual_reset;
    s->signaled = initial_state;
    return s;
#endif
}

HostSync host_create_semaphore(int32_t init, int32_t maxc) {
#ifdef _WIN32
    return CreateSemaphoreA(nullptr, init, maxc, nullptr);
#else
    auto s = std::make_shared<HostSyncObj>();
    s->kind = HostSyncObj::Kind::Semaphore;
    s->count = init;
    s->max_count = maxc;
    return s;
#endif
}

HostSync host_create_mutex(bool initial_owner) {
#ifdef _WIN32
    return CreateMutexA(nullptr, initial_owner ? TRUE : FALSE, nullptr);
#else
    auto s = std::make_shared<HostSyncObj>();
    s->kind = HostSyncObj::Kind::Mutant;
    if (initial_owner) {
        s->owner = std::this_thread::get_id();
        s->depth = 1;
    }
    return s;
#endif
}

void host_set_event(const HostSync& s) {
    if (!s) return;
#ifdef _WIN32
    SetEvent(s);
#else
    std::lock_guard<std::mutex> lk(g_sync_mtx);
    s->signaled = true;
    g_sync_cv.notify_all();
#endif
}

void host_reset_event(const HostSync& s) {
    if (!s) return;
#ifdef _WIN32
    ResetEvent(s);
#else
    std::lock_guard<std::mutex> lk(g_sync_mtx);
    s->signaled = false;
#endif
}

void host_pulse_event(const HostSync& s) {
    if (!s) return;
#ifdef _WIN32
    PulseEvent(s);
#else
    std::lock_guard<std::mutex> lk(g_sync_mtx);
    s->signaled = true;
    g_sync_cv.notify_all();
    s->signaled = false;
#endif
}

void host_release_semaphore(const HostSync& s, int32_t cnt, int32_t* prev) {
    if (!s) return;
#ifdef _WIN32
    LONG p = 0;
    ReleaseSemaphore(s, cnt, &p);
    if (prev) *prev = static_cast<int32_t>(p);
#else
    std::lock_guard<std::mutex> lk(g_sync_mtx);
    if (prev) *prev = s->count;
    s->count = std::min(s->max_count, s->count + cnt);
    g_sync_cv.notify_all();
#endif
}

void host_release_mutex(const HostSync& s) {
    if (!s) return;
#ifdef _WIN32
    ReleaseMutex(s);
#else
    std::lock_guard<std::mutex> lk(g_sync_mtx);
    if (s->owner == std::this_thread::get_id() && s->depth > 0) {
        if (--s->depth == 0) {
            s->owner = std::thread::id{};
            g_sync_cv.notify_all();
        }
    }
#endif
}

void host_close_sync(HostSync& s) {
    if (!s) return;
#ifdef _WIN32
    CloseHandle(s);
    s = nullptr;
#else
    s.reset();
#endif
}

#ifndef _WIN32
bool sync_is_ready(const HostSync& s) {
    if (!s) return true;
    switch (s->kind) {
        case HostSyncObj::Kind::Event: return s->signaled;
        case HostSyncObj::Kind::Semaphore: return s->count > 0;
        case HostSyncObj::Kind::Mutant:
            return s->depth == 0 || s->owner == std::this_thread::get_id();
    }
    return true;
}

void sync_consume(const HostSync& s) {
    if (!s) return;
    switch (s->kind) {
        case HostSyncObj::Kind::Event:
            if (!s->manual_reset) s->signaled = false;
            break;
        case HostSyncObj::Kind::Semaphore:
            if (s->count > 0) --s->count;
            break;
        case HostSyncObj::Kind::Mutant:
            s->owner = std::this_thread::get_id();
            ++s->depth;
            break;
    }
}
#endif

// Returns 0..count-1 or 0x102u (WAIT_TIMEOUT)
uint32_t host_wait_single(const HostSync& s, uint32_t ms) {
    if (!s) return 0u;
#ifdef _WIN32
    if (ms == 0) {
        return (WaitForSingleObject(s, 0) == WAIT_TIMEOUT) ? 0x102u : 0u;
    }
    uint32_t rem = ms;
    while (!g_krnl_shutting_down.load(std::memory_order_acquire)) {
        uint32_t step = (ms == INFINITE) ? 50u : std::min(rem, 50u);
        DWORD w = WaitForSingleObject(s, step);
        if (w != WAIT_TIMEOUT) return 0u;
        if (ms != INFINITE) {
            if (rem <= step) return 0x102u;
            rem -= step;
        }
    }
    return 0u;
#else
    std::unique_lock<std::mutex> lk(g_sync_mtx);
    auto pred = [&]() {
        return g_krnl_shutting_down.load(std::memory_order_acquire) || sync_is_ready(s);
    };
    if (ms == INFINITE) {
        g_sync_cv.wait(lk, pred);
    } else if (ms == 0) {
        if (!pred()) return 0x102u;
    } else {
        if (!g_sync_cv.wait_for(lk, std::chrono::milliseconds(ms), pred)) return 0x102u;
    }
    if (g_krnl_shutting_down.load(std::memory_order_acquire)) return 0u;
    sync_consume(s);
    return 0u;
#endif
}

uint32_t host_wait_multiple(uint32_t count, const HostSync* objs, bool wait_all, uint32_t ms) {
    if (count == 0) return 0u;
#ifdef _WIN32
    if (ms == 0) {
        DWORD w = WaitForMultipleObjects(count, objs, wait_all ? TRUE : FALSE, 0);
        if (w == WAIT_TIMEOUT) return 0x102u;
        if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + count) return w - WAIT_OBJECT_0;
        return 0u;
    }
    uint32_t rem = ms;
    while (!g_krnl_shutting_down.load(std::memory_order_acquire)) {
        uint32_t step = (ms == INFINITE) ? 50u : std::min(rem, 50u);
        DWORD w = WaitForMultipleObjects(count, objs, wait_all ? TRUE : FALSE, step);
        if (w != WAIT_TIMEOUT) {
            if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + count) return w - WAIT_OBJECT_0;
            return 0u;
        }
        if (ms != INFINITE) {
            if (rem <= step) return 0x102u;
            rem -= step;
        }
    }
    return 0u;
#else
    std::unique_lock<std::mutex> lk(g_sync_mtx);
    int hit = -1;
    auto pred = [&]() {
        if (g_krnl_shutting_down.load(std::memory_order_acquire)) return true;
        if (wait_all) {
            for (uint32_t i = 0; i < count; ++i) {
                if (!sync_is_ready(objs[i])) return false;
            }
            return true;
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                if (sync_is_ready(objs[i])) { hit = static_cast<int>(i); return true; }
            }
            return false;
        }
    };
    if (ms == INFINITE) {
        g_sync_cv.wait(lk, pred);
    } else if (ms == 0) {
        if (!pred()) return 0x102u;
    } else {
        if (!g_sync_cv.wait_for(lk, std::chrono::milliseconds(ms), pred)) return 0x102u;
    }
    if (g_krnl_shutting_down.load(std::memory_order_acquire)) return 0u;
    if (wait_all) {
        for (uint32_t i = 0; i < count; ++i) sync_consume(objs[i]);
        return 0u;
    }
    uint32_t idx = (hit >= 0) ? static_cast<uint32_t>(hit) : 0u;
    sync_consume(objs[idx]);
    return idx;
#endif
}

enum class ObjKind {
    File,
    SyncHandle,
    Thread,
};

struct KernelObj {
    ObjKind kind = ObjKind::SyncHandle;
    // File state
    bool is_iso = false;
    bool is_dir = false;
    bool delete_on_close = false;
    const XisoEntry* iso_entry = nullptr;
    std::string dev;
    std::string rel_path;
    HostFile host_handle = HOST_INVALID_FILE;
    uint64_t file_pos = 0;
    // Directory enumeration state
    std::vector<const XisoEntry*> dir_iso_items;
    std::vector<fs::directory_entry> dir_host_items;
    size_t dir_idx = 0;
    bool dir_started = false;
    std::string dir_pattern;
    // Sync object (Event / Semaphore / Mutex / Timer / Thread)
    HostSync sync_handle = nullptr;
    uint32_t guest_kobj_va = 0; // KEVENT/KSEMAPHORE/KTIMER guest address if any
    std::thread* thr = nullptr;
};

std::mutex g_krnl_mtx;
std::unordered_map<uint32_t, KernelObj*> g_handles;
std::unordered_map<uint32_t, HostSync> g_kobj_events;
std::unordered_map<uint32_t, std::recursive_mutex*> g_crit_secs;
std::unordered_map<std::string, std::string> g_symlinks;
uint32_t g_next_handle = 0x00010004u;
std::atomic<uint32_t> g_next_tid{2};
std::atomic<bool> g_tick_running{false};
std::thread g_tick_thread;

uint32_t KD_TICKCOUNT_VA = 0;

uint32_t alloc_handle(KernelObj* obj) {
    std::lock_guard<std::mutex> lk(g_krnl_mtx);
    uint32_t h = g_next_handle;
    g_next_handle += 4u;
    g_handles[h] = obj;
    return h;
}

KernelObj* get_handle(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_krnl_mtx);
    auto it = g_handles.find(h);
    return it != g_handles.end() ? it->second : nullptr;
}

HostSync get_handle_sync(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_krnl_mtx);
    auto it = g_handles.find(h);
    return (it != g_handles.end() && it->second) ? it->second->sync_handle : HostSync{};
}

std::recursive_mutex* get_or_create_cs(uint32_t cs_va) {
    std::lock_guard<std::mutex> lk(g_krnl_mtx);
    auto it = g_crit_secs.find(cs_va);
    if (it != g_crit_secs.end()) return it->second;
    auto* cs = new std::recursive_mutex();
    g_crit_secs[cs_va] = cs;
    return cs;
}

HostSync get_or_create_kobj_event(uint32_t kobj_va, bool manual_reset = true, bool initial_state = false) {
    std::lock_guard<std::mutex> lk(g_krnl_mtx);
    auto it = g_kobj_events.find(kobj_va);
    if (it != g_kobj_events.end()) return it->second;
    HostSync ev = host_create_event(manual_reset, initial_state);
    g_kobj_events[kobj_va] = ev;
    return ev;
}

std::string normalize_symlink_key(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        out.push_back((c == '/') ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    while (!out.empty() && out.back() == '\\') out.pop_back();
    return out;
}

std::string resolve_object_attrs(uint32_t obj_attr_va) {
    if (!obj_attr_va) return {};
    uint32_t root_dir = rd32(obj_attr_va + 0);
    uint32_t name_str_va = rd32(obj_attr_va + 4);
    std::string name = xbox_read_ansi_string(name_str_va);

    std::string prefix;
    if (root_dir) {
        if (auto* parent = get_handle(root_dir)) {
            if (!parent->dev.empty()) {
                if (parent->dev == "iso") prefix = "D:\\";
                else prefix = parent->dev + ":\\";
                if (!parent->rel_path.empty()) prefix += parent->rel_path + "\\";
            }
        }
    }
    std::string combined = prefix.empty() ? name : (prefix + name);
    std::string combined_lc = normalize_symlink_key(combined);
    std::lock_guard<std::mutex> lk(g_krnl_mtx);
    for (auto& kv : g_symlinks) {
        size_t klen = kv.first.size();
        if (klen > 0 && combined_lc.size() >= klen &&
            combined_lc.compare(0, klen, kv.first) == 0 &&
            (combined_lc.size() == klen || combined_lc[klen] == '\\')) {
            std::string tail = (combined.size() > klen) ? combined.substr(klen) : std::string{};
            combined = kv.second + tail;
            break;
        }
    }
    return combined;
}

#ifdef _WIN32
uint32_t win32_to_ntstatus(DWORD err) {
    switch (err) {
        case ERROR_SUCCESS: return STATUS_SUCCESS_XB;
        case ERROR_FILE_NOT_FOUND: return STATUS_OBJECT_NAME_NOT_FOUND_XB;
        case ERROR_PATH_NOT_FOUND: return STATUS_OBJECT_NAME_NOT_FOUND_XB;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: return STATUS_OBJECT_NAME_COLLISION_XB;
        case ERROR_HANDLE_EOF: return STATUS_END_OF_FILE_XB;
        case ERROR_NO_MORE_FILES: return STATUS_NO_MORE_FILES_XB;
        default: return 0xC0000001u;
    }
}
#else
uint32_t errno_to_ntstatus(int err) {
    switch (err) {
        case 0: return STATUS_SUCCESS_XB;
        case ENOENT:
        case ENOTDIR: return STATUS_OBJECT_NAME_NOT_FOUND_XB;
        case EEXIST: return STATUS_OBJECT_NAME_COLLISION_XB;
        default: return 0xC0000001u;
    }
}
#endif

uint64_t host_filetime_now() {
#ifdef _WIN32
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
#else
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    return 116444736000000000ull + ns / 100ull;
#endif
}

// ---------- Stall Watchdog & HLE Trace Ring ----------
struct HleTraceEntry {
    uint32_t tid;
    uint32_t last_pc;
    uint32_t arg0;
    uint32_t arg1;
    char op[48];
};
struct ThreadControl {
    HostSync resume_ev = nullptr;
    std::atomic<int32_t> suspend_cnt{0};
    uint32_t tid = 0;
    uint32_t handle = 0;
};
struct GuestThreadState {
    uint32_t tid = 0;
    CpuContext* ctx = nullptr;
    ThreadControl* ctrl = nullptr;
    const char* wait_op = nullptr;
    uint32_t wait_arg = 0;
    uint32_t wait_ms = 0;
};
static std::mutex g_diag_mtx;
static std::vector<GuestThreadState*> g_guest_threads;
static std::unordered_map<uint32_t, std::shared_ptr<ThreadControl>> g_thr_by_handle;
static std::unordered_map<uint32_t, std::shared_ptr<ThreadControl>> g_thr_by_tid;
static thread_local GuestThreadState* tl_thread_state = nullptr;
static thread_local ThreadControl* tl_self_ctrl = nullptr;
static thread_local uint32_t tl_tid = 0;
static HleTraceEntry g_trace_ring[64]{};
static uint32_t g_trace_write_idx = 0;
static std::atomic<uint64_t> g_kernel_tick_activity{0};
static void (*g_log_sink)(const char*) = nullptr;
static void (*g_thread_exit_hook)() = nullptr;

static void emit_log_sink(const char* msg) {
    if (g_log_sink && msg) g_log_sink(msg);
}

static void wait_if_self_suspended() {
    check_guest_shutdown();
    ThreadControl* tc = tl_self_ctrl;
    while (tc && tc->suspend_cnt.load(std::memory_order_acquire) > 0) {
        check_guest_shutdown();
        if (tl_thread_state) {
            tl_thread_state->wait_op = "Suspended";
            tl_thread_state->wait_arg = tc->handle;
            tl_thread_state->wait_ms = INFINITE;
        }
        host_wait_single(tc->resume_ev, INFINITE);
        if (tl_thread_state) tl_thread_state->wait_op = nullptr;
        check_guest_shutdown();
    }
}

static void record_hle_op(const CpuContext& ctx, const char* op, uint32_t a0 = 0, uint32_t a1 = 0) {
    std::lock_guard<std::mutex> lk(g_diag_mtx);
    auto& e = g_trace_ring[g_trace_write_idx++ & 63u];
    e.tid = tl_tid;
    e.last_pc = ctx.last_pc;
    e.arg0 = a0;
    e.arg1 = a1;
    std::snprintf(e.op, sizeof(e.op), "%s", op ? op : "?");
}

} // namespace
void hle_set_log_sink(void (*sink)(const char*)) {
    g_log_sink = sink;
}
void hle_set_thread_exit_hook(void (*hook)()) {
    g_thread_exit_hook = hook;
}
void hle_log_note(const char* fmt, ...) {
    char buf[1024]{};
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit_log_sink(buf);
}
void hle_kernel_notify_frame() {
    g_kernel_tick_activity.fetch_add(1, std::memory_order_relaxed);
}
void hle_kernel_dump_stall_state() {
    std::lock_guard<std::mutex> lk(g_diag_mtx);
    char line[512]{};
    std::snprintf(line, sizeof(line), "=== [STALL WATCHDOG DUMP] active_threads=%zu ===", g_guest_threads.size());
    std::printf("\n%s\n", line);
    emit_log_sink(line);
    for (auto* t : g_guest_threads) {
        if (!t || !t->ctx) continue;
        uint32_t esp = t->ctx->esp;
        uint32_t s0 = mem_ptr(esp) ? rd32(esp) : 0;
        uint32_t s1 = mem_ptr(esp + 4u) ? rd32(esp + 4u) : 0;
        uint32_t s2 = mem_ptr(esp + 8u) ? rd32(esp + 8u) : 0;
        std::snprintf(line, sizeof(line),
                      "  TID=%-3u last_pc=0x%08X esp=0x%08X [sp]={0x%08X,0x%08X,0x%08X} eax=0x%08X wait=%s(0x%X, ms=%u)",
                      t->tid, t->ctx->last_pc, esp, s0, s1, s2, t->ctx->eax,
                      t->wait_op ? t->wait_op : "running", t->wait_arg, t->wait_ms);
        std::printf("%s\n", line);
        emit_log_sink(line);
    }
    std::printf("--- Last HLE Trace Ring (oldest -> newest) ---\n");
    uint32_t start = (g_trace_write_idx >= 32u) ? (g_trace_write_idx - 32u) : 0u;
    for (uint32_t i = start; i < g_trace_write_idx; ++i) {
        const auto& e = g_trace_ring[i & 63u];
        std::snprintf(line, sizeof(line), "  [%u] tid=%u pc=0x%08X %-28s a0=0x%08X a1=0x%08X",
                      i, e.tid, e.last_pc, e.op, e.arg0, e.arg1);
        std::printf("%s\n", line);
        emit_log_sink(line);
    }
    std::printf("=== END STALL DUMP ===\n");
    emit_log_sink("=== END STALL DUMP ===");
    std::fflush(stdout);
}

uint64_t hle_kernel_get_stall_seq() {
    return g_kernel_tick_activity.load(std::memory_order_relaxed);
}
namespace {

// ---------- Critical Sections ----------
void hle_RtlInitializeCriticalSection(CpuContext& ctx) {
    uint32_t cs_va = hle_arg(ctx, 0);
    if (cs_va) {
        wr32(cs_va + 0, 0xFFFFFFFFu);
        wr32(cs_va + 4, 0u);
        wr32(cs_va + 8, 0u);
        wr32(cs_va + 12, 0u);
        get_or_create_cs(cs_va);
    }
    hle_ret_u32(ctx, 0, 4);
}
void hle_RtlEnterCriticalSection(CpuContext& ctx) {
    uint32_t cs_va = hle_arg(ctx, 0);
    if (cs_va) get_or_create_cs(cs_va)->lock();
    hle_ret_u32(ctx, 0, 4);
}
void hle_RtlLeaveCriticalSection(CpuContext& ctx) {
    uint32_t cs_va = hle_arg(ctx, 0);
    if (cs_va) get_or_create_cs(cs_va)->unlock();
    hle_ret_u32(ctx, 0, 4);
}

// ---------- Events / Semaphores / Mutants / Wait ----------
void hle_NtCreateEvent(CpuContext& ctx) {
    uint32_t ph = hle_arg(ctx, 0);
    uint32_t type = hle_arg(ctx, 2); // 0=Notification(manual), 1=Synchronization(auto)
    uint32_t init = hle_arg(ctx, 3) & 0xFFu;
    HostSync ev = host_create_event(type == 0, init != 0);
    auto* obj = new KernelObj();
    obj->kind = ObjKind::SyncHandle;
    obj->sync_handle = ev;
    uint32_t h = alloc_handle(obj);
    if (ph) wr32(ph, h);
    record_hle_op(ctx, "NtCreateEvent", h, init);
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 16);
}

void hle_NtSetEvent(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    uint32_t prev_ptr = hle_arg(ctx, 1);
    HostSync sync = get_handle_sync(h);
    if (sync) {
        if (prev_ptr) wr32(prev_ptr, 0);
        host_set_event(sync);
        hle_ret_u32(ctx, STATUS_SUCCESS_XB, 8);
    } else {
        hle_ret_u32(ctx, STATUS_INVALID_HANDLE_XB, 8);
    }
}

void hle_NtClearEvent(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    HostSync sync = get_handle_sync(h);
    if (sync) {
        host_reset_event(sync);
        hle_ret_u32(ctx, STATUS_SUCCESS_XB, 4);
    } else {
        hle_ret_u32(ctx, STATUS_INVALID_HANDLE_XB, 4);
    }
}

void hle_NtPulseEvent(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    uint32_t prev_ptr = hle_arg(ctx, 1);
    HostSync sync = get_handle_sync(h);
    if (sync) {
        if (prev_ptr) wr32(prev_ptr, 0);
        host_pulse_event(sync);
        hle_ret_u32(ctx, STATUS_SUCCESS_XB, 8);
    } else {
        hle_ret_u32(ctx, STATUS_INVALID_HANDLE_XB, 8);
    }
}

void hle_NtCreateSemaphore(CpuContext& ctx) {
    uint32_t ph = hle_arg(ctx, 0);
    int32_t init = static_cast<int32_t>(hle_arg(ctx, 2));
    int32_t maxc = static_cast<int32_t>(hle_arg(ctx, 3));
    if (maxc <= 0) maxc = 0x7FFFFFFF;
    auto* obj = new KernelObj();
    obj->kind = ObjKind::SyncHandle;
    obj->sync_handle = host_create_semaphore(init, maxc);
    if (ph) wr32(ph, alloc_handle(obj));
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 16);
}

void hle_NtReleaseSemaphore(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    int32_t cnt = static_cast<int32_t>(hle_arg(ctx, 1));
    uint32_t prev_ptr = hle_arg(ctx, 2);
    HostSync sync = get_handle_sync(h);
    int32_t prev = 0;
    if (sync) {
        host_release_semaphore(sync, cnt, &prev);
        if (prev_ptr) wr32(prev_ptr, static_cast<uint32_t>(prev));
        hle_ret_u32(ctx, STATUS_SUCCESS_XB, 12);
    } else {
        hle_ret_u32(ctx, STATUS_INVALID_HANDLE_XB, 12);
    }
}

void hle_NtCreateMutant(CpuContext& ctx) {
    uint32_t ph = hle_arg(ctx, 0);
    uint32_t init = hle_arg(ctx, 2) & 0xFFu;
    auto* obj = new KernelObj();
    obj->kind = ObjKind::SyncHandle;
    obj->sync_handle = host_create_mutex(init != 0);
    if (ph) wr32(ph, alloc_handle(obj));
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 12);
}

void hle_NtReleaseMutant(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    uint32_t prev_ptr = hle_arg(ctx, 1);
    HostSync sync = get_handle_sync(h);
    if (sync) {
        host_release_mutex(sync);
        if (prev_ptr) wr32(prev_ptr, 0);
        hle_ret_u32(ctx, STATUS_SUCCESS_XB, 8);
    } else {
        hle_ret_u32(ctx, STATUS_INVALID_HANDLE_XB, 8);
    }
}

uint32_t timeout_ptr_to_ms(uint32_t to_va) {
    if (!to_va) return INFINITE;
    int64_t val = static_cast<int64_t>(rd64(to_va));
    if (val <= 0) return static_cast<uint32_t>((-val) / 10000ll);
    return 0;
}

void hle_NtWaitForSingleObjectEx(CpuContext& ctx) {
    check_guest_shutdown();
    uint32_t h = hle_arg(ctx, 0);
    uint32_t to_va = hle_arg(ctx, 3);
    HostSync sync = get_handle_sync(h);
    uint32_t ms = timeout_ptr_to_ms(to_va);
    if (!sync) {
        hle_ret_u32(ctx, STATUS_INVALID_HANDLE_XB, 16);
        return;
    }
    record_hle_op(ctx, "NtWaitSingleEx", h, ms);
    if (tl_thread_state) { tl_thread_state->wait_op = "NtWaitSingleEx"; tl_thread_state->wait_arg = h; tl_thread_state->wait_ms = ms; }
    uint32_t w = host_wait_single(sync, ms);
    if (tl_thread_state) tl_thread_state->wait_op = nullptr;
    check_guest_shutdown();
    hle_ret_u32(ctx, (w == 0x102u) ? 0x00000102u : STATUS_SUCCESS_XB, 16);
}

void hle_NtWaitForSingleObject(CpuContext& ctx) {
    check_guest_shutdown();
    uint32_t h = hle_arg(ctx, 0);
    uint32_t to_va = hle_arg(ctx, 2);
    HostSync sync = get_handle_sync(h);
    uint32_t ms = timeout_ptr_to_ms(to_va);
    if (!sync) {
        hle_ret_u32(ctx, STATUS_INVALID_HANDLE_XB, 12);
        return;
    }
    record_hle_op(ctx, "NtWaitSingle", h, ms);
    if (tl_thread_state) { tl_thread_state->wait_op = "NtWaitSingle"; tl_thread_state->wait_arg = h; tl_thread_state->wait_ms = ms; }
    uint32_t w = host_wait_single(sync, ms);
    if (tl_thread_state) tl_thread_state->wait_op = nullptr;
    check_guest_shutdown();
    hle_ret_u32(ctx, (w == 0x102u) ? 0x00000102u : STATUS_SUCCESS_XB, 12);
}

void hle_NtWaitForMultipleObjectsEx(CpuContext& ctx) {
    check_guest_shutdown();
    uint32_t count = hle_arg(ctx, 0);
    uint32_t handles_va = hle_arg(ctx, 1);
    uint32_t wait_type = hle_arg(ctx, 2); // 0=All, 1=Any
    uint32_t to_va = hle_arg(ctx, 4);
    std::vector<HostSync> wh(count);
    {
        std::lock_guard<std::mutex> lk(g_krnl_mtx);
        for (uint32_t i = 0; i < count; ++i) {
            auto it = g_handles.find(rd32(handles_va + i * 4u));
            wh[i] = (it != g_handles.end() && it->second) ? it->second->sync_handle : HostSync{};
        }
    }
    uint32_t ms = timeout_ptr_to_ms(to_va);
    record_hle_op(ctx, "NtWaitMultiEx", count, ms);
    if (tl_thread_state) { tl_thread_state->wait_op = "NtWaitMultiEx"; tl_thread_state->wait_arg = count; tl_thread_state->wait_ms = ms; }
    uint32_t w = host_wait_multiple(count, wh.data(), wait_type == 0, ms);
    if (tl_thread_state) tl_thread_state->wait_op = nullptr;
    check_guest_shutdown();
    hle_ret_u32(ctx, w, 20);
}

void hle_NtSignalAndWaitForSingleObjectEx(CpuContext& ctx) {
    check_guest_shutdown();
    uint32_t sig_h = hle_arg(ctx, 0);
    uint32_t wait_h = hle_arg(ctx, 1);
    uint32_t to_va = hle_arg(ctx, 3);
    HostSync sig_sync = get_handle_sync(sig_h);
    HostSync wait_sync = get_handle_sync(wait_h);
    if (sig_sync) host_set_event(sig_sync);
    if (!wait_sync) { hle_ret_u32(ctx, STATUS_INVALID_HANDLE_XB, 16); return; }
    uint32_t ms = timeout_ptr_to_ms(to_va);
    record_hle_op(ctx, "NtSignalAndWait", sig_h, wait_h);
    if (tl_thread_state) { tl_thread_state->wait_op = "NtSignalAndWait"; tl_thread_state->wait_arg = wait_h; tl_thread_state->wait_ms = ms; }
    uint32_t w = host_wait_single(wait_sync, ms);
    if (tl_thread_state) tl_thread_state->wait_op = nullptr;
    check_guest_shutdown();
    hle_ret_u32(ctx, (w == 0x102u) ? 0x00000102u : STATUS_SUCCESS_XB, 16);
}

void hle_KeSetEvent(CpuContext& ctx) {
    uint32_t kev_va = hle_arg(ctx, 0);
    HostSync ev = get_or_create_kobj_event(kev_va, true, false);
    if (kev_va) wr32(kev_va + 4, 1);
    host_set_event(ev);
    record_hle_op(ctx, "KeSetEvent", kev_va, 0);
    hle_ret_u32(ctx, 0, 12);
}

void hle_KeWaitForSingleObject(CpuContext& ctx) {
    check_guest_shutdown();
    uint32_t kobj_va = hle_arg(ctx, 0);
    uint32_t to_va = hle_arg(ctx, 4);
    HostSync ev = get_or_create_kobj_event(kobj_va, true, false);
    uint32_t ms = timeout_ptr_to_ms(to_va);
    record_hle_op(ctx, "KeWaitSingle", kobj_va, ms);
    if (tl_thread_state) { tl_thread_state->wait_op = "KeWaitSingle"; tl_thread_state->wait_arg = kobj_va; tl_thread_state->wait_ms = ms; }
    uint32_t w = host_wait_single(ev, ms);
    if (tl_thread_state) tl_thread_state->wait_op = nullptr;
    check_guest_shutdown();
    hle_ret_u32(ctx, (w == 0x102u) ? 0x00000102u : STATUS_SUCCESS_XB, 20);
}

void hle_KeWaitForMultipleObjects(CpuContext& ctx) {
    check_guest_shutdown();
    uint32_t count = hle_arg(ctx, 0);
    uint32_t objs_va = hle_arg(ctx, 1);
    uint32_t wait_type = hle_arg(ctx, 2);
    uint32_t to_va = hle_arg(ctx, 5);
    std::vector<HostSync> wh(count);
    for (uint32_t i = 0; i < count; ++i) wh[i] = get_or_create_kobj_event(rd32(objs_va + i * 4u), true, false);
    uint32_t ms = timeout_ptr_to_ms(to_va);
    record_hle_op(ctx, "KeWaitMulti", count, ms);
    if (tl_thread_state) { tl_thread_state->wait_op = "KeWaitMulti"; tl_thread_state->wait_arg = count; tl_thread_state->wait_ms = ms; }
    uint32_t w = host_wait_multiple(count, wh.data(), wait_type == 0, ms);
    if (tl_thread_state) tl_thread_state->wait_op = nullptr;
    check_guest_shutdown();
    hle_ret_u32(ctx, w, 28);
}

void hle_KeInitializeTimerEx(CpuContext& ctx) {
    uint32_t t_va = hle_arg(ctx, 0);
    get_or_create_kobj_event(t_va, true, false);
    hle_ret(ctx, 8);
}
void hle_KeSetTimer(CpuContext& ctx) {
    uint32_t t_va = hle_arg(ctx, 0);
    host_set_event(get_or_create_kobj_event(t_va, true, true));
    hle_ret_u32(ctx, 1, 16);
}
void hle_KeSetTimerEx(CpuContext& ctx) {
    uint32_t t_va = hle_arg(ctx, 0);
    host_set_event(get_or_create_kobj_event(t_va, true, true));
    hle_ret_u32(ctx, 1, 20);
}
void hle_KeCancelTimer(CpuContext& ctx) {
    hle_ret_u32(ctx, 1, 4);
}
void hle_NtCreateTimer(CpuContext& ctx) {
    uint32_t ph = hle_arg(ctx, 0);
    uint32_t timer_type = hle_arg(ctx, 3); // 0=Notification, 1=Synchronization
    auto* obj = new KernelObj();
    obj->kind = ObjKind::SyncHandle;
    obj->sync_handle = host_create_event(timer_type == 0, false);
    if (ph) wr32(ph, alloc_handle(obj));
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 12);
}
void hle_NtSetTimerEx(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    if (HostSync s = get_handle_sync(h)) host_set_event(s);
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 28);
}
void hle_NtCancelTimer(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    if (HostSync s = get_handle_sync(h)) host_reset_event(s);
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 8);
}

void hle_KeDelayExecutionThread(CpuContext& ctx) {
    check_guest_shutdown();
    uint32_t to_va = hle_arg(ctx, 2);
    uint32_t ms = to_va ? timeout_ptr_to_ms(to_va) : 0;
    host_sleep_ms(ms);
    wait_if_self_suspended();
    check_guest_shutdown();
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 12);
}
void hle_NtYieldExecution(CpuContext& ctx) { check_guest_shutdown(); std::this_thread::yield(); wait_if_self_suspended(); hle_ret_u32(ctx, 0, 0); }
void hle_KeStallExecutionProcessor(CpuContext& ctx) { check_guest_shutdown(); std::this_thread::yield(); hle_ret(ctx, 4); }
void hle_KfRaiseIrql(CpuContext& ctx) { hle_ret_u32(ctx, 0, 4); }
void hle_KfLowerIrql(CpuContext& ctx) {
    // __fastcall in x86 kernel: cl = NewIrql, 0 stack args!
    (void)ctx.cl();
    hle_ret(ctx, 0);
}
void hle_KeRaiseIrqlToDpcLevel(CpuContext& ctx) { hle_ret_u32(ctx, 0, 0); }
void hle_KeInitializeDpc(CpuContext& ctx) { hle_ret(ctx, 12); }
void hle_KeInsertQueueDpc(CpuContext& ctx) { hle_ret_u32(ctx, 1, 12); }
void hle_KeRemoveQueueDpc(CpuContext& ctx) { hle_ret_u32(ctx, 0, 4); }
void hle_KeInitializeInterrupt(CpuContext& ctx) { hle_ret(ctx, 28); }
void hle_KeConnectInterrupt(CpuContext& ctx) { hle_ret_u32(ctx, 1, 4); }
void hle_KeDisconnectInterrupt(CpuContext& ctx) { hle_ret(ctx, 4); }
void hle_KeSynchronizeExecution(CpuContext& ctx) { hle_ret_u32(ctx, 1, 12); }
void hle_KeSaveFloatingPointState(CpuContext& ctx) { hle_ret_u32(ctx, 0, 4); }
void hle_KeRestoreFloatingPointState(CpuContext& ctx) { hle_ret_u32(ctx, 0, 4); }

// ---------- Memory Management ----------
void hle_NtAllocateVirtualMemory(CpuContext& ctx) {
    uint32_t pbase = hle_arg(ctx, 0);
    uint32_t psize = hle_arg(ctx, 2);
    uint32_t base = pbase ? rd32(pbase) : 0;
    uint32_t sz   = psize ? rd32(psize) : 0;
    uint32_t aligned = (sz + 4095u) & ~4095u;
    if (base == 0) {
        base = xbox_guest_alloc(aligned, 4096u, true);
    }
    if (!base) { hle_ret_u32(ctx, 0xC0000017u, 20); return; }
    if (pbase) wr32(pbase, base);
    if (psize) wr32(psize, aligned);
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 20);
}
void hle_NtFreeVirtualMemory(CpuContext& ctx) {
    uint32_t pbase = hle_arg(ctx, 0);
    uint32_t ftype = hle_arg(ctx, 2);
    if ((ftype & 0x8000u) && pbase) xbox_guest_free(rd32(pbase));
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 12);
}
void hle_NtProtectVirtualMemory(CpuContext& ctx) { hle_ret_u32(ctx, STATUS_SUCCESS_XB, 16); }
void hle_NtQueryVirtualMemory(CpuContext& ctx) {
    uint32_t base = hle_arg(ctx, 0);
    uint32_t info = hle_arg(ctx, 1);
    if (info) {
        wr32(info + 0, base & ~4095u);
        wr32(info + 4, base & ~4095u);
        wr32(info + 8, 0x04);
        wr32(info + 12, 0x100000u);
        wr32(info + 16, 0x1000);
        wr32(info + 20, 0x04);
        wr32(info + 24, 0x20000);
    }
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 8);
}

void hle_MmAllocateContiguousMemory(CpuContext& ctx) {
    uint32_t sz = hle_arg(ctx, 0);
    hle_ret_u32(ctx, xbox_guest_alloc(sz, 4096u, true), 4);
}
void hle_MmAllocateContiguousMemoryEx(CpuContext& ctx) {
    uint32_t sz = hle_arg(ctx, 0);
    uint32_t align = hle_arg(ctx, 3);
    if (align < 4096u) align = 4096u;
    hle_ret_u32(ctx, xbox_guest_alloc(sz, align, true), 20);
}
void hle_MmFreeContiguousMemory(CpuContext& ctx) {
    xbox_guest_free(hle_arg(ctx, 0));
    hle_ret(ctx, 4);
}
void hle_MmAllocateSystemMemory(CpuContext& ctx) {
    uint32_t sz = hle_arg(ctx, 0);
    hle_ret_u32(ctx, xbox_guest_alloc(sz, 4096u, true), 8);
}
void hle_MmFreeSystemMemory(CpuContext& ctx) {
    xbox_guest_free(hle_arg(ctx, 0));
    hle_ret_u32(ctx, 0, 8);
}
void hle_MmQueryAllocationSize(CpuContext& ctx) {
    uint32_t va = hle_arg(ctx, 0);
    uint32_t sz = xbox_guest_alloc_size(va);
    hle_ret_u32(ctx, sz ? sz : 4096u, 4);
}
void hle_MmQueryStatistics(CpuContext& ctx) {
    uint32_t st = hle_arg(ctx, 0);
    uint32_t tot = 0, avail = 0;
    xbox_guest_query_stats(&tot, &avail);
    if (st) {
        std::memset(mem_ptr(st), 0, 0x24);
        wr32(st + 0x00, 0x24);
        wr32(st + 0x04, tot);
        wr32(st + 0x08, avail);
    }
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 4);
}
void hle_MmGetPhysicalAddress(CpuContext& ctx) { hle_ret_u32(ctx, hle_arg(ctx, 0), 4); }
void hle_MmLockUnlockBufferPages(CpuContext& ctx) { hle_ret(ctx, 12); }
void hle_MmLockUnlockPhysicalPage(CpuContext& ctx) { hle_ret(ctx, 8); }
void hle_MmPersistContiguousMemory(CpuContext& ctx) { hle_ret(ctx, 12); }
void hle_MmQueryAddressProtect(CpuContext& ctx) { hle_ret_u32(ctx, 0x04, 4); }
void hle_MmSetAddressProtect(CpuContext& ctx) { hle_ret(ctx, 12); }
void hle_MmClaimGpuInstanceMemory(CpuContext& ctx) {
    uint32_t sz = hle_arg(ctx, 0);
    uint32_t out = hle_arg(ctx, 1);
    if (out) wr32(out, sz);
    hle_ret_u32(ctx, xbox_guest_alloc(sz ? sz : 0x10000, 4096, true), 8);
}
void hle_ExAllocatePool(CpuContext& ctx) {
    hle_ret_u32(ctx, xbox_guest_alloc(hle_arg(ctx, 0), 16, true), 4);
}
void hle_ExAllocatePoolWithTag(CpuContext& ctx) {
    hle_ret_u32(ctx, xbox_guest_alloc(hle_arg(ctx, 0), 16, true), 8);
}
void hle_ExFreePool(CpuContext& ctx) {
    xbox_guest_free(hle_arg(ctx, 0));
    hle_ret(ctx, 4);
}
void hle_ExQueryPoolBlockSize(CpuContext& ctx) {
    uint32_t sz = xbox_guest_alloc_size(hle_arg(ctx, 0));
    hle_ret_u32(ctx, sz ? sz : 64u, 4);
}

// ---------- File I/O & XISO ----------
static uint32_t open_file_common(uint32_t ph, uint32_t obj_attr_va, uint32_t io_sb_va,
                                 uint32_t disp, uint32_t options, bool create_mode) {
    std::string raw_path = resolve_object_attrs(obj_attr_va);
    std::string dev, rel;
    if (!XisoVfs::instance().resolve_path(raw_path, dev, rel)) {
        if (io_sb_va) { wr32(io_sb_va, STATUS_OBJECT_NAME_NOT_FOUND_XB); wr32(io_sb_va + 4, 0); }
        return STATUS_OBJECT_NAME_NOT_FOUND_XB;
    }

    if (dev == "iso") {
        // Empty rel == root directory
        bool want_dir = (options & 0x01u) != 0;
        const XisoEntry* entry = rel.empty() ? nullptr : XisoVfs::instance().find_iso_entry(rel);
        if (!rel.empty() && !entry) {
            if (g_hle_trace) std::printf("[IO] ISO miss: %s (rel='%s')\n", raw_path.c_str(), rel.c_str());
            if (io_sb_va) { wr32(io_sb_va, STATUS_OBJECT_NAME_NOT_FOUND_XB); wr32(io_sb_va + 4, 0); }
            return STATUS_OBJECT_NAME_NOT_FOUND_XB;
        }
        auto* obj = new KernelObj();
        obj->kind = ObjKind::File;
        obj->is_iso = true;
        obj->is_dir = rel.empty() ? true : (entry->is_dir || want_dir);
        obj->iso_entry = entry;
        obj->dev = dev;
        obj->rel_path = rel;
        obj->file_pos = 0;
        uint32_t h = alloc_handle(obj);
        if (ph) wr32(ph, h);
        if (io_sb_va) { wr32(io_sb_va, STATUS_SUCCESS_XB); wr32(io_sb_va + 4, 1); }
        if (g_hle_trace) std::printf("[IO] ISO open: '%s' (rel='%s') -> h=0x%X (sz=%u)\n", raw_path.c_str(), rel.c_str(), h, entry ? entry->size : 0);
        return STATUS_SUCCESS_XB;
    }

    // Writable host storage (T:\, U:\, Z:\)
    std::string host = XisoVfs::instance().host_path_for(dev, rel);
    std::error_code ec;
    bool delete_on_close = (options & 0x1000u) != 0; // FILE_DELETE_ON_CLOSE
    bool non_dir_only = (options & 0x0040u) != 0;    // FILE_NON_DIRECTORY_FILE
    bool want_dir = (options & 0x01u) != 0 || (!non_dir_only && fs::is_directory(host, ec));
    bool existed_before = fs::exists(host, ec);
    if (want_dir) {
        if (create_mode && disp == 2 && existed_before) {
            if (io_sb_va) { wr32(io_sb_va, STATUS_OBJECT_NAME_COLLISION_XB); wr32(io_sb_va + 4, 0); }
            return STATUS_OBJECT_NAME_COLLISION_XB;
        }
        bool created_dir = false;
        if (create_mode && (disp == 2 || disp == 3 || disp == 5)) {
            if (!existed_before) created_dir = fs::create_directories(host, ec);
        } else if (!existed_before) {
            if (io_sb_va) { wr32(io_sb_va, STATUS_OBJECT_NAME_NOT_FOUND_XB); wr32(io_sb_va + 4, 0); }
            return STATUS_OBJECT_NAME_NOT_FOUND_XB;
        }
        auto* obj = new KernelObj();
        obj->kind = ObjKind::File;
        obj->is_iso = false;
        obj->is_dir = true;
        obj->delete_on_close = delete_on_close;
        obj->dev = dev;
        obj->rel_path = rel;
        uint32_t h = alloc_handle(obj);
        if (ph) wr32(ph, h);
        if (io_sb_va) { wr32(io_sb_va, STATUS_SUCCESS_XB); wr32(io_sb_va + 4, created_dir ? 2u : 1u); }
        return STATUS_SUCCESS_XB;
    }

    if (create_mode && (disp == 2 || disp == 3 || disp == 5)) {
        fs::path hp(host);
        if (hp.has_parent_path()) fs::create_directories(hp.parent_path());
    }
#ifdef _WIN32
    DWORD wdisp = OPEN_EXISTING;
    if (create_mode) {
        switch (disp) {
            case 0: wdisp = CREATE_ALWAYS; break;     // FILE_SUPERSEDE
            case 1: wdisp = OPEN_EXISTING; break;     // FILE_OPEN
            case 2: wdisp = CREATE_NEW; break;        // FILE_CREATE
            case 3: wdisp = OPEN_ALWAYS; break;       // FILE_OPEN_IF
            case 4: wdisp = TRUNCATE_EXISTING; break; // FILE_OVERWRITE
            case 5: wdisp = CREATE_ALWAYS; break;     // FILE_OVERWRITE_IF
            default: wdisp = OPEN_ALWAYS; break;
        }
    }
    HostFile fh = CreateFileA(host.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, wdisp,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == HOST_INVALID_FILE) {
        uint32_t st = win32_to_ntstatus(GetLastError());
        if (io_sb_va) { wr32(io_sb_va, st); wr32(io_sb_va + 4, 0); }
        return st;
    }
#else
    int flags = O_RDWR;
    if (create_mode) {
        switch (disp) {
            case 0: flags |= O_CREAT | O_TRUNC; break;
            case 1: break;
            case 2: flags |= O_CREAT | O_EXCL; break;
            case 3: flags |= O_CREAT; break;
            case 4: flags |= O_TRUNC; break;
            case 5: flags |= O_CREAT | O_TRUNC; break;
            default: flags |= O_CREAT; break;
        }
    }
    HostFile fh = ::open(host.c_str(), flags, 0644);
    if (fh < 0) {
        uint32_t st = errno_to_ntstatus(errno);
        if (io_sb_va) { wr32(io_sb_va, st); wr32(io_sb_va + 4, 0); }
        return st;
    }
#endif
    auto* obj = new KernelObj();
    obj->kind = ObjKind::File;
    obj->is_iso = false;
    obj->delete_on_close = delete_on_close;
    obj->dev = dev;
    obj->rel_path = rel;
    obj->host_handle = fh;
    uint32_t io_info = 1u;
    if (create_mode) {
        if (disp == 0) io_info = existed_before ? 0u : 2u;
        else if (disp == 2 || ((disp == 3 || disp == 5) && !existed_before)) io_info = 2u;
        else if (disp == 4 || (disp == 5 && existed_before)) io_info = 3u;
    }
    uint32_t h = alloc_handle(obj);
    if (ph) wr32(ph, h);
    if (io_sb_va) { wr32(io_sb_va, STATUS_SUCCESS_XB); wr32(io_sb_va + 4, io_info); }
    return STATUS_SUCCESS_XB;
}

void hle_NtCreateFile(CpuContext& ctx) {
    uint32_t st = open_file_common(hle_arg(ctx, 0), hle_arg(ctx, 2), hle_arg(ctx, 3),
                                   hle_arg(ctx, 7), hle_arg(ctx, 8), true);
    hle_ret_u32(ctx, st, 36);
}
void hle_NtOpenFile(CpuContext& ctx) {
    uint32_t st = open_file_common(hle_arg(ctx, 0), hle_arg(ctx, 2), hle_arg(ctx, 3),
                                   1, hle_arg(ctx, 5), false);
    hle_ret_u32(ctx, st, 24);
}
void hle_NtReadFile(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    uint32_t io_sb = hle_arg(ctx, 4);
    uint32_t buf = hle_arg(ctx, 5);
    uint32_t len = hle_arg(ctx, 6);
    uint32_t off_va = hle_arg(ctx, 7);
    auto* obj = get_handle(h);
    if (!obj || obj->kind != ObjKind::File) {
        hle_ret_u32(ctx, STATUS_INVALID_HANDLE_XB, 32);
        return;
    }
    if (off_va) {
        int64_t o = static_cast<int64_t>(rd64(off_va));
        if (o >= 0) obj->file_pos = static_cast<uint64_t>(o);
    }
    uint32_t got = 0;
    uint32_t st = STATUS_SUCCESS_XB;
    if (obj->is_iso) {
        if (!obj->iso_entry || obj->file_pos >= obj->iso_entry->size) {
            st = STATUS_END_OF_FILE_XB;
        } else {
            XisoVfs::instance().read_iso_range(obj->iso_entry, obj->file_pos, mem_ptr(buf), len, &got);
            obj->file_pos += got;
            if (got == 0 && len > 0) st = STATUS_END_OF_FILE_XB;
        }
    } else {
#ifdef _WIN32
        LARGE_INTEGER li{}; li.QuadPart = static_cast<LONGLONG>(obj->file_pos);
        SetFilePointerEx(obj->host_handle, li, nullptr, FILE_BEGIN);
        DWORD rd = 0;
        if (!ReadFile(obj->host_handle, mem_ptr(buf), len, &rd, nullptr)) {
            st = win32_to_ntstatus(GetLastError());
        } else {
            got = rd;
            obj->file_pos += got;
            if (got == 0 && len > 0) st = STATUS_END_OF_FILE_XB;
        }
#else
        ssize_t r = ::pread(obj->host_handle, mem_ptr(buf), len, static_cast<off_t>(obj->file_pos));
        if (r < 0) {
            st = errno_to_ntstatus(errno);
        } else {
            got = static_cast<uint32_t>(r);
            obj->file_pos += got;
            if (got == 0 && len > 0) st = STATUS_END_OF_FILE_XB;
        }
#endif
    }
    if (io_sb) { wr32(io_sb, st); wr32(io_sb + 4, got); }
    if (st == STATUS_SUCCESS_XB && got >= 32 && !obj->is_iso && !obj->is_dir && obj->dev == "U") {
        static std::atomic<uint64_t> s_last_save_ms{0};
        uint64_t now_ms = host_ticks_ms();
        uint64_t prev_ms = s_last_save_ms.load(std::memory_order_relaxed);
        if (now_ms - prev_ms > 2000 && s_last_save_ms.compare_exchange_weak(prev_ms, now_ms)) {
            rl_ach_save_loaded();
        }
    }
    hle_ret_u32(ctx, st, 32);
}

void hle_NtWriteFile(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    uint32_t io_sb = hle_arg(ctx, 4);
    uint32_t buf = hle_arg(ctx, 5);
    uint32_t len = hle_arg(ctx, 6);
    uint32_t off_va = hle_arg(ctx, 7);
    auto* obj = get_handle(h);
    if (!obj || obj->kind != ObjKind::File || obj->is_iso || obj->host_handle == HOST_INVALID_FILE) {
        hle_ret_u32(ctx, STATUS_INVALID_HANDLE_XB, 32);
        return;
    }
    if (off_va) {
        int64_t o = static_cast<int64_t>(rd64(off_va));
        if (o >= 0) obj->file_pos = static_cast<uint64_t>(o);
    }
    uint32_t wr = 0;
    uint32_t st = STATUS_SUCCESS_XB;
#ifdef _WIN32
    LARGE_INTEGER li{}; li.QuadPart = static_cast<LONGLONG>(obj->file_pos);
    SetFilePointerEx(obj->host_handle, li, nullptr, FILE_BEGIN);
    DWORD dwr = 0;
    st = WriteFile(obj->host_handle, mem_ptr(buf), len, &dwr, nullptr)
             ? STATUS_SUCCESS_XB : win32_to_ntstatus(GetLastError());
    wr = dwr;
#else
    ssize_t w = ::pwrite(obj->host_handle, mem_ptr(buf), len, static_cast<off_t>(obj->file_pos));
    if (w < 0) st = errno_to_ntstatus(errno);
    else wr = static_cast<uint32_t>(w);
#endif
    obj->file_pos += wr;
    if (io_sb) { wr32(io_sb, st); wr32(io_sb + 4, wr); }
    hle_ret_u32(ctx, st, 32);
}

void hle_NtQueryInformationFile(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    uint32_t io_sb = hle_arg(ctx, 1);
    uint32_t info = hle_arg(ctx, 2);
    uint32_t len = hle_arg(ctx, 3);
    uint32_t cls = hle_arg(ctx, 4);
    auto* obj = get_handle(h);
    if (!obj || obj->kind != ObjKind::File) {
        hle_ret_u32(ctx, STATUS_INVALID_HANDLE_XB, 20);
        return;
    }
    uint64_t fsz = 0;
    if (obj->is_iso && obj->iso_entry) fsz = obj->iso_entry->size;
    else if (obj->host_handle != HOST_INVALID_FILE) {
#ifdef _WIN32
        LARGE_INTEGER sz{}; GetFileSizeEx(obj->host_handle, &sz); fsz = static_cast<uint64_t>(sz.QuadPart);
#else
        struct stat st{};
        if (::fstat(obj->host_handle, &st) == 0) fsz = static_cast<uint64_t>(st.st_size);
#endif
    }
    if (info && len) std::memset(mem_ptr(info), 0, len);
    if (cls == 5 && info && len >= 24) { // FileStandardInformation
        wr64(info + 0, (fsz + 2047ull) & ~2047ull);
        wr64(info + 8, fsz);
        wr32(info + 16, 1);
        wr8(info + 20, 0);
        wr8(info + 21, obj->is_dir ? 1 : 0);
    } else if (cls == 14 && info && len >= 8) { // FilePositionInformation
        wr64(info, obj->file_pos);
    } else if (cls == 34 && info && len >= 56) { // FileNetworkOpenInformation
        wr64(info + 32, (fsz + 2047ull) & ~2047ull);
        wr64(info + 40, fsz);
        wr32(info + 48, obj->is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
    }
    if (io_sb) { wr32(io_sb, STATUS_SUCCESS_XB); wr32(io_sb + 4, len); }
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 20);
}

void hle_NtSetInformationFile(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    uint32_t io_sb = hle_arg(ctx, 1);
    uint32_t info = hle_arg(ctx, 2);
    uint32_t cls = hle_arg(ctx, 4);
    auto* obj = get_handle(h);
    if (!obj || obj->kind != ObjKind::File) {
        hle_ret_u32(ctx, STATUS_INVALID_HANDLE_XB, 20);
        return;
    }
    if (cls == 13 && info && !obj->is_iso) { // FileDispositionInformation
        obj->delete_on_close = (rd8(info) != 0);
    } else if (cls == 14 && info) { // FilePositionInformation
        obj->file_pos = rd64(info);
    } else if ((cls == 19 || cls == 20) && info && !obj->is_iso && obj->host_handle != HOST_INVALID_FILE) {
        // FileAllocationInformation(19) / FileEndOfFileInformation(20)
#ifdef _WIN32
        LARGE_INTEGER li{}; li.QuadPart = static_cast<LONGLONG>(rd64(info));
        SetFilePointerEx(obj->host_handle, li, nullptr, FILE_BEGIN);
        SetEndOfFile(obj->host_handle);
#else
        if (::ftruncate(obj->host_handle, static_cast<off_t>(rd64(info))) != 0) {}
#endif
    }
    if (io_sb) { wr32(io_sb, STATUS_SUCCESS_XB); wr32(io_sb + 4, 0); }
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 20);
}

} // namespace

bool hle_file_read(uint32_t handle, void* dst, uint32_t len, uint32_t* out_got) {
    if (out_got) *out_got = 0;
    auto* obj = get_handle(handle);
    if (!obj || obj->kind != ObjKind::File) return false;
    uint32_t got = 0;
    if (obj->is_iso) {
        if (obj->iso_entry && obj->file_pos < obj->iso_entry->size) {
            XisoVfs::instance().read_iso_range(obj->iso_entry, obj->file_pos, dst, len, &got);
            obj->file_pos += got;
        }
    } else if (obj->host_handle != HOST_INVALID_FILE) {
#ifdef _WIN32
        LARGE_INTEGER li{}; li.QuadPart = static_cast<LONGLONG>(obj->file_pos);
        SetFilePointerEx(obj->host_handle, li, nullptr, FILE_BEGIN);
        DWORD rd = 0;
        if (ReadFile(obj->host_handle, dst, len, &rd, nullptr)) {
            got = rd;
            obj->file_pos += got;
        } else {
            return false;
        }
#else
        ssize_t r = ::pread(obj->host_handle, dst, len, static_cast<off_t>(obj->file_pos));
        if (r >= 0) {
            got = static_cast<uint32_t>(r);
            obj->file_pos += got;
        } else {
            return false;
        }
#endif
    } else {
        return false;
    }
    if (out_got) *out_got = got;
    return true;
}

uint64_t hle_file_size(uint32_t handle) {
    auto* obj = get_handle(handle);
    if (!obj || obj->kind != ObjKind::File) return 0;
    if (obj->is_iso && obj->iso_entry) return obj->iso_entry->size;
    if (obj->host_handle != HOST_INVALID_FILE) {
#ifdef _WIN32
        LARGE_INTEGER sz{};
        if (GetFileSizeEx(obj->host_handle, &sz)) return static_cast<uint64_t>(sz.QuadPart);
#else
        struct stat st{};
        if (::fstat(obj->host_handle, &st) == 0) return static_cast<uint64_t>(st.st_size);
#endif
    }
    return 0;
}

bool hle_file_seek(uint32_t handle, int64_t offset, uint32_t origin, uint64_t* out_pos) {
    auto* obj = get_handle(handle);
    if (!obj || obj->kind != ObjKind::File) return false;
    uint64_t fsz = hle_file_size(handle);
    int64_t base = 0;
    if (origin == 1) base = static_cast<int64_t>(obj->file_pos);
    else if (origin == 2) base = static_cast<int64_t>(fsz);
    int64_t np = base + offset;
    if (np < 0) np = 0;
    obj->file_pos = static_cast<uint64_t>(np);
    if (out_pos) *out_pos = obj->file_pos;
    return true;
}

bool hle_file_read_bink_from_handle(uint32_t handle, std::vector<uint8_t>& out_bik, uint64_t* out_off) {
    out_bik.clear();
    auto* obj = get_handle(handle);
    if (!obj || obj->kind != ObjKind::File) return false;
    uint64_t start_off = obj->file_pos;
    if (out_off) *out_off = start_off;

    auto read_at = [&](uint64_t pos, void* dst, uint32_t len, uint32_t* got) -> bool {
        *got = 0;
        if (obj->is_iso) {
            if (!obj->iso_entry || pos >= obj->iso_entry->size) return false;
            return XisoVfs::instance().read_iso_range(obj->iso_entry, pos, dst, len, got);
        } else if (obj->host_handle != HOST_INVALID_FILE) {
#ifdef _WIN32
            LARGE_INTEGER li{}; li.QuadPart = static_cast<LONGLONG>(pos);
            SetFilePointerEx(obj->host_handle, li, nullptr, FILE_BEGIN);
            DWORD rd = 0;
            BOOL ok = ReadFile(obj->host_handle, dst, len, &rd, nullptr);
            *got = rd;
            return ok != FALSE;
#else
            ssize_t r = ::pread(obj->host_handle, dst, len, static_cast<off_t>(pos));
            if (r < 0) return false;
            *got = static_cast<uint32_t>(r);
            return true;
#endif
        }
        return false;
    };

    uint8_t hdr[48]{};
    uint32_t got = 0;
    if (!read_at(start_off, hdr, sizeof(hdr), &got) || got < 32 || std::memcmp(hdr, "BIK", 3) != 0) {
        return false;
    }
    uint32_t raw_sz = 0;
    std::memcpy(&raw_sz, hdr + 4, 4);
    uint64_t total_sz = static_cast<uint64_t>(raw_sz) + 8ull;
    if (total_sz < 48ull || total_sz > 256ull * 1024ull * 1024ull) return false;

    out_bik.resize(static_cast<size_t>(total_sz));
    uint32_t total_got = 0;
    if (!read_at(start_off, out_bik.data(), static_cast<uint32_t>(total_sz), &total_got) || total_got < 48u) {
        out_bik.clear();
        return false;
    }
    out_bik.resize(total_got);
    return true;
}

namespace {

void hle_NtQueryVolumeInformationFile(CpuContext& ctx) {
    uint32_t io_sb = hle_arg(ctx, 1);
    uint32_t info = hle_arg(ctx, 2);
    uint32_t len = hle_arg(ctx, 3);
    uint32_t cls = hle_arg(ctx, 4);
    if (info && len) std::memset(mem_ptr(info), 0, len);
    if (cls == 3 && info && len >= 24) { // FileFsSizeInformation
        wr64(info + 0, 1048576ull); // total allocation units
        wr64(info + 8, 524288ull);  // available
        wr32(info + 16, 32);        // sectors per unit
        wr32(info + 20, 512);       // bytes per sector
    } else if (cls == 5 && info && len >= 12) { // FileFsAttributeInformation
        wr32(info + 0, 0);
        wr32(info + 4, 42);
        wr32(info + 8, 4);
        if (len >= 16) std::memcpy(mem_ptr(info + 12), "FATX", 4);
    }
    if (io_sb) { wr32(io_sb, STATUS_SUCCESS_XB); wr32(io_sb + 4, len); }
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 20);
}

void hle_NtQueryFullAttributesFile(CpuContext& ctx) {
    uint32_t oa = hle_arg(ctx, 0);
    uint32_t info = hle_arg(ctx, 1);
    std::string raw = resolve_object_attrs(oa);
    std::string dev, rel;
    if (!XisoVfs::instance().resolve_path(raw, dev, rel)) {
        hle_ret_u32(ctx, STATUS_OBJECT_NAME_NOT_FOUND_XB, 8);
        return;
    }
    if (info) std::memset(mem_ptr(info), 0, 36);
    if (dev == "iso") {
        const XisoEntry* e = rel.empty() ? nullptr : XisoVfs::instance().find_iso_entry(rel);
        if (!rel.empty() && !e) {
            hle_ret_u32(ctx, STATUS_OBJECT_NAME_NOT_FOUND_XB, 8);
            return;
        }
        if (info) {
            uint64_t sz = e ? e->size : 0;
            wr64(info + 16, (sz + 2047ull) & ~2047ull);
            wr64(info + 24, sz);
            wr32(info + 32, (!e || e->is_dir) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY);
        }
        hle_ret_u32(ctx, STATUS_SUCCESS_XB, 8);
        return;
    }
    std::string hp = XisoVfs::instance().host_path_for(dev, rel);
    uint64_t sz = 0;
    bool is_dir = false;
    uint32_t attrs = FILE_ATTRIBUTE_NORMAL;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(hp.c_str(), GetFileExInfoStandard, &fad)) {
        hle_ret_u32(ctx, STATUS_OBJECT_NAME_NOT_FOUND_XB, 8);
        return;
    }
    sz = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    attrs = fad.dwFileAttributes;
    (void)is_dir;
#else
    struct stat st{};
    if (::stat(hp.c_str(), &st) != 0) {
        hle_ret_u32(ctx, STATUS_OBJECT_NAME_NOT_FOUND_XB, 8);
        return;
    }
    is_dir = S_ISDIR(st.st_mode);
    sz = is_dir ? 0ull : static_cast<uint64_t>(st.st_size);
    attrs = is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
#endif
    if (info) {
        wr64(info + 16, (sz + 4095ull) & ~4095ull);
        wr64(info + 24, sz);
        wr32(info + 32, attrs);
    }
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 8);
}

static bool wildcard_match_ci(const char* pat, const char* str) {
    if (!pat || !*pat || std::strcmp(pat, "*.*") == 0 || std::strcmp(pat, "*") == 0) return true;
    const char* cp = nullptr;
    const char* mp = nullptr;
    while (*str && *pat != '*') {
        if (std::tolower(static_cast<unsigned char>(*pat)) != std::tolower(static_cast<unsigned char>(*str)) && *pat != '?') {
            return false;
        }
        ++pat; ++str;
    }
    while (*str) {
        if (*pat == '*') {
            if (!*++pat) return true;
            mp = pat;
            cp = str + 1;
        } else if (std::tolower(static_cast<unsigned char>(*pat)) == std::tolower(static_cast<unsigned char>(*str)) || *pat == '?') {
            ++pat; ++str;
        } else {
            pat = mp;
            str = cp++;
        }
    }
    while (*pat == '*') ++pat;
    return !*pat;
}

void hle_NtQueryDirectoryFile(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    uint32_t io_sb = hle_arg(ctx, 4);
    uint32_t buf = hle_arg(ctx, 5);
    uint32_t len = hle_arg(ctx, 6);
    uint32_t fn_str_va = hle_arg(ctx, 8);
    // NT BOOLEAN parameters only define the low byte; upper stack bytes can hold garbage.
    bool return_single = (hle_arg(ctx, 7) & 0xFFu) != 0;
    bool restart = (hle_arg(ctx, 9) & 0xFFu) != 0;
    (void)return_single;
    auto* obj = get_handle(h);
    if (!obj || obj->kind != ObjKind::File) {
        hle_ret_u32(ctx, STATUS_INVALID_HANDLE_XB, 40);
        return;
    }
    std::string pat = fn_str_va ? xbox_read_ansi_string(fn_str_va) : "*.*";
    if (pat.empty()) pat = "*.*";
    if (!obj->dir_started || restart) {
        obj->dir_started = true;
        obj->dir_idx = 0;
        obj->dir_pattern = pat;
        if (obj->is_iso) {
            obj->dir_iso_items = XisoVfs::instance().list_iso_dir(obj->rel_path, pat);
        } else {
            obj->dir_host_items.clear();
            std::string hp = XisoVfs::instance().host_path_for(obj->dev, obj->rel_path);
            std::error_code ec;
            if (fs::exists(hp, ec) && fs::is_directory(hp, ec)) {
                for (auto& de : fs::directory_iterator(hp, ec)) {
                    std::string fn = de.path().filename().string();
                    if (wildcard_match_ci(pat.c_str(), fn.c_str())) {
                        obj->dir_host_items.push_back(de);
                    }
                }
            }
        }
    }
    if (obj->is_iso) {
        if (obj->dir_idx >= obj->dir_iso_items.size()) {
            if (io_sb) { wr32(io_sb, STATUS_NO_MORE_FILES_XB); wr32(io_sb + 4, 0); }
            hle_ret_u32(ctx, STATUS_NO_MORE_FILES_XB, 40);
            return;
        }
        const XisoEntry* e = obj->dir_iso_items[obj->dir_idx++];
        std::memset(mem_ptr(buf), 0, std::min<uint32_t>(len, 0x44));
        uint32_t nlen = static_cast<uint32_t>(e->name.size());
        uint64_t now_ft = host_filetime_now();
        wr32(buf + 0, 0); // NextEntryOffset (single entry per call on Xbox XAPI)
        wr64(buf + 8, now_ft);
        wr64(buf + 16, now_ft);
        wr64(buf + 24, now_ft);
        wr64(buf + 32, now_ft);
        wr64(buf + 40, e->size);
        wr64(buf + 48, (e->size + 2047ull) & ~2047ull);
        wr32(buf + 56, e->attrs);
        wr32(buf + 60, nlen);
        if (64 + nlen <= len) std::memcpy(mem_ptr(buf + 64), e->name.data(), nlen);
        if (io_sb) { wr32(io_sb, STATUS_SUCCESS_XB); wr32(io_sb + 4, 64 + nlen); }
        hle_ret_u32(ctx, STATUS_SUCCESS_XB, 40);
        return;
    }
    if (obj->dir_idx >= obj->dir_host_items.size()) {
        if (io_sb) { wr32(io_sb, STATUS_NO_MORE_FILES_XB); wr32(io_sb + 4, 0); }
        hle_ret_u32(ctx, STATUS_NO_MORE_FILES_XB, 40);
        return;
    }
    const auto& de = obj->dir_host_items[obj->dir_idx++];
    std::string nm = de.path().filename().string();
    uint64_t sz = de.is_directory() ? 0 : de.file_size();
    uint32_t nlen = static_cast<uint32_t>(nm.size());
    std::memset(mem_ptr(buf), 0, std::min<uint32_t>(len, 0x44));
    uint64_t ct = host_filetime_now(), at = ct, wt = ct;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExA(de.path().string().c_str(), GetFileExInfoStandard, &fad)) {
        ct = (static_cast<uint64_t>(fad.ftCreationTime.dwHighDateTime) << 32) | fad.ftCreationTime.dwLowDateTime;
        at = (static_cast<uint64_t>(fad.ftLastAccessTime.dwHighDateTime) << 32) | fad.ftLastAccessTime.dwLowDateTime;
        wt = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) | fad.ftLastWriteTime.dwLowDateTime;
    }
#else
    struct stat st{};
    if (::stat(de.path().string().c_str(), &st) == 0) {
        auto to_ft = [](time_t t) -> uint64_t {
            return (static_cast<uint64_t>(t) + 11644473600ull) * 10000000ull;
        };
        ct = to_ft(st.st_ctime);
        at = to_ft(st.st_atime);
        wt = to_ft(st.st_mtime);
    }
#endif
    wr64(buf + 8, ct);
    wr64(buf + 16, at);
    wr64(buf + 24, wt);
    wr64(buf + 32, wt);
    wr64(buf + 40, sz);
    wr64(buf + 48, (sz + 4095ull) & ~4095ull);
    wr32(buf + 56, de.is_directory() ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
    wr32(buf + 60, nlen);
    if (64 + nlen <= len) std::memcpy(mem_ptr(buf + 64), nm.data(), nlen);
    if (io_sb) { wr32(io_sb, STATUS_SUCCESS_XB); wr32(io_sb + 4, 64 + nlen); }
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 40);
}

void hle_NtDeleteFile(CpuContext& ctx) {
    std::string raw = resolve_object_attrs(hle_arg(ctx, 0));
    std::string dev, rel;
    if (XisoVfs::instance().resolve_path(raw, dev, rel) && dev != "iso") {
        std::string hp = XisoVfs::instance().host_path_for(dev, rel);
        std::error_code ec;
        fs::remove(hp, ec);
    }
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 4);
}
void hle_NtFlushBuffersFile(CpuContext& ctx) { hle_ret_u32(ctx, STATUS_SUCCESS_XB, 8); }
void hle_NtClose(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    KernelObj* o = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_krnl_mtx);
        auto it = g_handles.find(h);
        if (it != g_handles.end()) {
            o = it->second;
            g_handles.erase(it);
        }
    }
    if (o) {
        bool del = (o->kind == ObjKind::File) && o->delete_on_close && !o->is_iso && !o->dev.empty();
        bool del_dir = o->is_dir;
        std::string del_path = del ? XisoVfs::instance().host_path_for(o->dev, o->rel_path) : std::string{};
#ifdef _WIN32
        if (o->host_handle != HOST_INVALID_FILE) CloseHandle(o->host_handle);
#else
        if (o->host_handle != HOST_INVALID_FILE) ::close(o->host_handle);
#endif
        if (o->sync_handle && o->kind != ObjKind::Thread) host_close_sync(o->sync_handle);
        delete o;
        if (del && !del_path.empty()) {
            std::error_code ec;
            if (del_dir) fs::remove_all(del_path, ec);
            else fs::remove(del_path, ec);
        }
    }
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 4);
}

void hle_IoCreateSymbolicLink(CpuContext& ctx) {
    std::string link = normalize_symlink_key(xbox_read_ansi_string(hle_arg(ctx, 0)));
    std::string target = xbox_read_ansi_string(hle_arg(ctx, 1));
    for (char& c : target) if (c == '/') c = '\\';
    while (!target.empty() && target.back() == '\\') target.pop_back();
    std::lock_guard<std::mutex> lk(g_krnl_mtx);
    if (!link.empty()) g_symlinks[link] = target;
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 8);
}
void hle_IoDeleteSymbolicLink(CpuContext& ctx) {
    std::string link = normalize_symlink_key(xbox_read_ansi_string(hle_arg(ctx, 0)));
    std::lock_guard<std::mutex> lk(g_krnl_mtx);
    g_symlinks.erase(link);
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 4);
}
void hle_NtOpenSymbolicLinkObject(CpuContext& ctx) {
    uint32_t ph = hle_arg(ctx, 0);
    std::string s = resolve_object_attrs(hle_arg(ctx, 1));
    auto* o = new KernelObj();
    o->dev = "sym";
    o->rel_path = s;
    if (ph) wr32(ph, alloc_handle(o));
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 8);
}
void hle_NtQuerySymbolicLinkObject(CpuContext& ctx) {
    uint32_t out_ansi = hle_arg(ctx, 1);
    if (out_ansi) {
        const char* p = "\\Device\\Cdrom0";
        uint32_t buf = rd32(out_ansi + 4);
        uint16_t maxl = rd16(out_ansi + 2);
        if (buf && maxl > 14) {
            std::memcpy(mem_ptr(buf), p, 15);
            wr16(out_ansi + 0, 14);
        }
    }
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 12);
}

// ---------- Threads ----------
void hle_PsCreateSystemThreadEx(CpuContext& ctx) {
    uint32_t ph = hle_arg(ctx, 0);
    uint32_t stack_sz = hle_arg(ctx, 2);
    uint32_t tls_sz   = hle_arg(ctx, 3);
    uint32_t ptid     = hle_arg(ctx, 4);
    uint32_t start_c1 = hle_arg(ctx, 5);
    uint32_t start_c2 = hle_arg(ctx, 6);
    uint32_t suspended = hle_arg(ctx, 7) & 1u;
    uint32_t start_fn = hle_arg(ctx, 9);

    if (stack_sz < 0x00100000u) stack_sz = 0x00100000u;
    static std::once_flag s_prot_once;
    std::call_once(s_prot_once, []() { xbox_mem_protect_readonly(); });
    uint32_t stack_base = xbox_guest_alloc(stack_sz, 4096u, true);
    uint32_t tid = g_next_tid.fetch_add(1u);
    uint32_t teb_va = xbox_alloc_teb(stack_base + stack_sz, stack_base, tls_sz, tid);

    auto* obj = new KernelObj();
    obj->kind = ObjKind::Thread;
    obj->sync_handle = host_create_event(true, false);
    uint32_t h = alloc_handle(obj);
    if (ph) wr32(ph, h);
    if (ptid) wr32(ptid, tid);

    auto tc = std::make_shared<ThreadControl>();
    tc->suspend_cnt.store(suspended ? 1 : 0, std::memory_order_release);
    tc->resume_ev = host_create_event(true, !suspended);
    tc->tid = tid;
    tc->handle = h;
    {
        std::lock_guard<std::mutex> dlk(g_diag_mtx);
        g_thr_by_handle[h] = tc;
        g_thr_by_tid[tid] = tc;
    }

    HostSync done_ev = obj->sync_handle;
    std::thread([=]() {
        CpuContext tctx{};
        tctx.fs_base = teb_va;
        tctx.esp = stack_base + stack_sz - 64u;
        tl_tid = tid;
        tl_self_ctrl = tc.get();
        GuestThreadState gts{};
        gts.tid = tid;
        gts.ctx = &tctx;
        gts.ctrl = tc.get();
        tl_thread_state = &gts;
        {
            std::lock_guard<std::mutex> dlk(g_diag_mtx);
            g_guest_threads.push_back(&gts);
        }
        try {
            wait_if_self_suspended();
            // XapiThreadStartup(StartContext1, StartContext2) stdcall
            tctx.push32(start_c2);
            tctx.push32(start_c1);
            tctx.push32(0xDEAD1001u);
            if (g_hle_trace) {
                std::printf("[THR %u] Starting routine=0x%08X ctx1=0x%08X ctx2=0x%08X susp=%u stack=0x%08X\n",
                            tid, start_fn, start_c1, start_c2, suspended, tctx.esp);
            }
            record_hle_op(tctx, "ThreadStart", start_fn, suspended);
            recomp_dispatch_call(tctx, start_fn);
        } catch (const GuestPowerOff&) {
            app_request_quit();
        }
        if (g_thread_exit_hook) g_thread_exit_hook();
        tl_thread_state = nullptr;
        tl_self_ctrl = nullptr;
        xbox_free_teb(teb_va);
        xbox_guest_free(stack_base);
        host_set_event(done_ev);
        {
            std::lock_guard<std::mutex> dlk(g_diag_mtx);
            g_guest_threads.erase(
                std::remove(g_guest_threads.begin(), g_guest_threads.end(), &gts),
                g_guest_threads.end());
            g_thr_by_handle.erase(h);
            g_thr_by_tid.erase(tid);
        }
        g_thr_exit_cv.notify_all();
    }).detach();
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 40);
}

void hle_PsTerminateSystemThread(CpuContext& ctx) {
    // Signal thread termination cleanly by returning from thread stack frame or exit
    hle_ret(ctx, 4);
}

// ---------- Time / Settings / Strings / Crypto ----------
void hle_KeQueryPerformanceCounter(CpuContext& ctx) {
#ifdef _WIN32
    LARGE_INTEGER li{}; QueryPerformanceCounter(&li);
    hle_ret_u64(ctx, static_cast<uint64_t>(li.QuadPart), 0);
#else
    static const auto t0 = std::chrono::steady_clock::now();
    uint64_t ticks = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count() / 100);
    hle_ret_u64(ctx, ticks + 1000000ull, 0);
#endif
}
void hle_KeQueryPerformanceFrequency(CpuContext& ctx) {
#ifdef _WIN32
    LARGE_INTEGER li{}; QueryPerformanceFrequency(&li);
    hle_ret_u64(ctx, static_cast<uint64_t>(li.QuadPart), 0);
#else
    hle_ret_u64(ctx, 10000000ull, 0);
#endif
}
void hle_KeQuerySystemTime(CpuContext& ctx) {
    uint32_t p = hle_arg(ctx, 0);
    if (p) wr64(p, host_filetime_now());
    hle_ret(ctx, 4);
}
void hle_KeQueryInterruptTime(CpuContext& ctx) {
    uint64_t ms = host_ticks_ms();
    hle_ret_u64(ctx, ms * 10000ull, 0);
}

void hle_ExQueryNonVolatileSetting(CpuContext& ctx) {
    uint32_t idx = hle_arg(ctx, 0);
    uint32_t ptype = hle_arg(ctx, 1);
    uint32_t pval = hle_arg(ctx, 2);
    uint32_t vlen = hle_arg(ctx, 3);
    uint32_t rlen = hle_arg(ctx, 4);
    uint32_t out_v = 0;
    switch (idx) {
        case 0x07: out_v = 1; break;          // Language: English
        case 0x08: out_v = 0x000B0001u; break;// VideoFlags: WIDESCREEN (0x10000) | HDTV_720p (0x20000) | HDTV_480p (0x80000)
        case 0x09: out_v = 0; break;          // AudioFlags: Stereo
        case 0x0A: out_v = 6; break;          // Parental control: MAX (disabled)
        case 0x100: out_v = 0; break;         // Misc flags
        case 0x103: out_v = 0x00010100u; break; // VideoStandard: XC_VIDEO_STANDARD_NTSC_M (1)
        default: out_v = 0; break;
    }
    if (ptype) wr32(ptype, 4);
    if (pval && vlen >= 4) wr32(pval, out_v);
    if (rlen) wr32(rlen, 4);
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 20);
}

void hle_RtlInitAnsiString(CpuContext& ctx) {
    uint32_t dst = hle_arg(ctx, 0);
    uint32_t src = hle_arg(ctx, 1);
    uint16_t len = src ? static_cast<uint16_t>(xbox_read_cstr(src, 4096).size()) : 0;
    if (dst) {
        wr16(dst + 0, len);
        wr16(dst + 2, src ? static_cast<uint16_t>(len + 1) : 0);
        wr32(dst + 4, src);
    }
    hle_ret(ctx, 8);
}
void hle_RtlInitUnicodeString(CpuContext& ctx) {
    uint32_t dst = hle_arg(ctx, 0);
    uint32_t src = hle_arg(ctx, 1);
    uint16_t len = 0;
    if (src) {
        while (rd16(src + len) != 0 && len < 4096) len += 2;
    }
    if (dst) {
        wr16(dst + 0, len);
        wr16(dst + 2, src ? static_cast<uint16_t>(len + 2) : 0);
        wr32(dst + 4, src);
    }
    hle_ret(ctx, 8);
}
void hle_RtlNtStatusToDosError(CpuContext& ctx) {
    uint32_t st = hle_arg(ctx, 0);
    uint32_t err = ERROR_GEN_FAILURE;
    if (st == 0) err = 0;
    else if (st == STATUS_OBJECT_NAME_NOT_FOUND_XB || st == STATUS_NO_SUCH_FILE_XB) err = ERROR_FILE_NOT_FOUND;
    else if (st == STATUS_OBJECT_NAME_COLLISION_XB) err = ERROR_ALREADY_EXISTS;
    else if (st == STATUS_END_OF_FILE_XB) err = ERROR_HANDLE_EOF;
    else if (st == STATUS_NO_MORE_FILES_XB) err = ERROR_NO_MORE_FILES;
    hle_ret_u32(ctx, err, 4);
}
void hle_RtlCompareMemory(CpuContext& ctx) {
    const uint8_t* a = mem_ptr(hle_arg(ctx, 0));
    const uint8_t* b = mem_ptr(hle_arg(ctx, 1));
    uint32_t len = hle_arg(ctx, 2);
    uint32_t i = 0;
    while (i < len && a[i] == b[i]) ++i;
    hle_ret_u32(ctx, i, 12);
}
void hle_RtlCompareMemoryUlong(CpuContext& ctx) {
    const uint32_t* a = reinterpret_cast<const uint32_t*>(mem_ptr(hle_arg(ctx, 0)));
    uint32_t bytes = hle_arg(ctx, 1);
    uint32_t pat = hle_arg(ctx, 2);
    uint32_t i = 0;
    while ((i + 4) <= bytes && a[i / 4] == pat) i += 4;
    hle_ret_u32(ctx, i, 12);
}
void hle_RtlFillMemoryUlong(CpuContext& ctx) {
    uint32_t* d = reinterpret_cast<uint32_t*>(mem_ptr(hle_arg(ctx, 0)));
    uint32_t bytes = hle_arg(ctx, 1);
    uint32_t pat = hle_arg(ctx, 2);
    for (uint32_t i = 0; i < bytes / 4; ++i) d[i] = pat;
    hle_ret(ctx, 12);
}
void hle_RtlTimeFieldsToTime(CpuContext& ctx) {
    uint32_t tf = hle_arg(ctx, 0);
    uint32_t tm = hle_arg(ctx, 1);
#ifdef _WIN32
    SYSTEMTIME st{};
    st.wYear = rd16(tf + 0); st.wMonth = rd16(tf + 2); st.wDay = rd16(tf + 4);
    st.wHour = rd16(tf + 6); st.wMinute = rd16(tf + 8); st.wSecond = rd16(tf + 10);
    st.wMilliseconds = rd16(tf + 12);
    FILETIME ft{};
    BOOL ok = SystemTimeToFileTime(&st, &ft);
    if (ok && tm) wr64(tm, (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    hle_ret_u32(ctx, ok ? 1u : 0u, 8);
#else
    struct tm t{};
    t.tm_year = static_cast<int>(rd16(tf + 0)) - 1900;
    t.tm_mon  = static_cast<int>(rd16(tf + 2)) - 1;
    t.tm_mday = static_cast<int>(rd16(tf + 4));
    t.tm_hour = static_cast<int>(rd16(tf + 6));
    t.tm_min  = static_cast<int>(rd16(tf + 8));
    t.tm_sec  = static_cast<int>(rd16(tf + 10));
    time_t sec = ::timegm(&t);
    bool ok = (sec != static_cast<time_t>(-1));
    if (ok && tm) {
        uint64_t ft = (static_cast<uint64_t>(sec) + 11644473600ull) * 10000000ull +
                      static_cast<uint64_t>(rd16(tf + 12)) * 10000ull;
        wr64(tm, ft);
    }
    hle_ret_u32(ctx, ok ? 1u : 0u, 8);
#endif
}
void hle_RtlTimeToTimeFields(CpuContext& ctx) {
    uint32_t tm = hle_arg(ctx, 0);
    uint32_t tf = hle_arg(ctx, 1);
    uint64_t q = tm ? rd64(tm) : 0;
#ifdef _WIN32
    FILETIME ft{ static_cast<DWORD>(q), static_cast<DWORD>(q >> 32) };
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    if (tf) {
        wr16(tf + 0, st.wYear); wr16(tf + 2, st.wMonth); wr16(tf + 4, st.wDay);
        wr16(tf + 6, st.wHour); wr16(tf + 8, st.wMinute); wr16(tf + 10, st.wSecond);
        wr16(tf + 12, st.wMilliseconds); wr16(tf + 14, st.wDayOfWeek);
    }
#else
    int64_t sec64 = static_cast<int64_t>(q / 10000000ull) - 11644473600ll;
    if (sec64 < 0) sec64 = 0;
    time_t sec = static_cast<time_t>(sec64);
    struct tm t{};
    ::gmtime_r(&sec, &t);
    if (tf) {
        wr16(tf + 0, static_cast<uint16_t>(t.tm_year + 1900));
        wr16(tf + 2, static_cast<uint16_t>(t.tm_mon + 1));
        wr16(tf + 4, static_cast<uint16_t>(t.tm_mday));
        wr16(tf + 6, static_cast<uint16_t>(t.tm_hour));
        wr16(tf + 8, static_cast<uint16_t>(t.tm_min));
        wr16(tf + 10, static_cast<uint16_t>(t.tm_sec));
        wr16(tf + 12, static_cast<uint16_t>((q / 10000ull) % 1000ull));
        wr16(tf + 14, static_cast<uint16_t>(t.tm_wday));
    }
#endif
    hle_ret(ctx, 8);
}
void hle_RtlCompareString(CpuContext& ctx) {
    std::string a = xbox_read_ansi_string(hle_arg(ctx, 0));
    std::string b = xbox_read_ansi_string(hle_arg(ctx, 1));
    uint32_t ci = hle_arg(ctx, 2);
    int r = ci ? ci_strcmp(a.c_str(), b.c_str()) : std::strcmp(a.c_str(), b.c_str());
    hle_ret_u32(ctx, static_cast<uint32_t>(r), 12);
}
void hle_RtlEqualString(CpuContext& ctx) {
    std::string a = xbox_read_ansi_string(hle_arg(ctx, 0));
    std::string b = xbox_read_ansi_string(hle_arg(ctx, 1));
    uint32_t ci = hle_arg(ctx, 2);
    int r = ci ? ci_strcmp(a.c_str(), b.c_str()) : std::strcmp(a.c_str(), b.c_str());
    hle_ret_u32(ctx, r == 0 ? 1u : 0u, 12);
}

// Crypto / AV / Misc stubs
void hle_XcSHAInit(CpuContext& ctx) {
    uint32_t p = hle_arg(ctx, 0);
    if (p) std::memset(mem_ptr(p), 0, 116);
    hle_ret(ctx, 4);
}
void hle_XcSHAUpdate(CpuContext& ctx) {
    uint32_t c = hle_arg(ctx, 0), data = hle_arg(ctx, 1), sz = hle_arg(ctx, 2);
    if (c && data && sz) {
        uint32_t h = rd32(c) ^ 0x9E3779B9u;
        for (uint32_t i = 0; i < sz; ++i) h = h * 16777619u ^ rd8(data + i);
        wr32(c, h);
    }
    hle_ret(ctx, 12);
}
void hle_XcSHAFinal(CpuContext& ctx) {
    uint32_t c = hle_arg(ctx, 0), dig = hle_arg(ctx, 1);
    if (dig) {
        uint32_t h = c ? rd32(c) : 0x12345678u;
        for (int i = 0; i < 5; ++i) { h = h * 0x45D9F3Bu + i; wr32(dig + i * 4, h); }
    }
    hle_ret(ctx, 8);
}
void hle_XcHMAC(CpuContext& ctx) {
    uint32_t out = hle_arg(ctx, 6);
    if (out) std::memset(mem_ptr(out), 0xA5, 20);
    hle_ret(ctx, 28);
}

void hle_AvGetSavedDataAddress(CpuContext& ctx) {
    hle_ret_u32(ctx, HleKernelDataLayout::KERNEL_EXPORTS_BASE + 0x800u, 0);
}
void hle_AvSetSavedDataAddress(CpuContext& ctx) { hle_ret(ctx, 4); }
void hle_AvSetDisplayMode(CpuContext& ctx) {
    record_hle_op(ctx, "AvSetDisplayMode", hle_arg(ctx, 1), hle_arg(ctx, 2));
    hle_ret_u32(ctx, 0, 24);
}
void hle_AvSendTVEncoderOption(CpuContext& ctx) {
    uint32_t opt = hle_arg(ctx, 1);
    uint32_t pres = hle_arg(ctx, 3);
    if (opt == 6u && pres) {
        // HDTV Component 60Hz NTSC-M | Widescreen | 720p | 480p (matches DisplayModeTable 0x004B0104)
        wr32(pres, 0x004B0104u);
    }
    record_hle_op(ctx, "AvSendTVEncoderOption", opt, pres ? rd32(pres) : 0);
    hle_ret(ctx, 16);
}

void hle_DbgPrint(CpuContext& ctx) {
    std::string fmt = xbox_read_cstr(hle_arg(ctx, 0), 256);
    if (g_hle_trace) std::printf("[DbgPrint] %s\n", fmt.c_str());
    hle_ret_u32(ctx, 0, 0); // cdecl!
}
void hle_KeBugCheck(CpuContext& ctx) {
    std::fprintf(stderr, "[KERNEL] KeBugCheck(0x%08X) from last_pc=0x%08X!\n", hle_arg(ctx, 0), ctx.last_pc);
    hle_ret(ctx, 4);
}
void hle_HalReturnToFirmware(CpuContext& ctx) {
    hle_log_note("[KERNEL] HalReturnToFirmware(%u)", hle_arg(ctx, 0));
    throw GuestPowerOff{};
}

#define DEF_STUB(NAME, RET, POP) \
    static void hle_##NAME(CpuContext& c) { record_hle_op(c, "STUB:" #NAME, hle_arg(c, 0), 0); hle_ret_u32(c, (RET), (POP)); }
#define DEF_VOID_STUB(NAME, POP) \
    static void hle_##NAME(CpuContext& c) { record_hle_op(c, "STUB:" #NAME, hle_arg(c, 0), 0); hle_ret(c, (POP)); }

DEF_STUB(ObReferenceObjectByHandle, 0, 12)
DEF_STUB(ObfDereferenceObject, 0, 0) // fastcall ecx!
DEF_STUB(ObOpenObjectByName, STATUS_OBJECT_NAME_NOT_FOUND_XB, 28)
DEF_STUB(ObReferenceObjectByName, STATUS_OBJECT_NAME_NOT_FOUND_XB, 20)
DEF_STUB(NtDuplicateObject, 0, 16)
DEF_STUB(NtFsControlFile, 0, 40)
DEF_STUB(NtDeviceIoControlFile, 0, 40)
DEF_STUB(NtReadFileScatter, 0, 36)
DEF_STUB(NtWriteFileGather, 0, 36)
DEF_STUB(NtCreateIoCompletion, 0, 16)
DEF_STUB(NtSetIoCompletion, 0, 20)
DEF_STUB(NtRemoveIoCompletion, 0, 20)
DEF_STUB(NtQueryDirectoryFile_unused, 0, 40)
DEF_STUB(NtQueueApcThread, 0, 20)
static void hle_NtSuspendThread(CpuContext& ctx) {
    check_guest_shutdown();
    uint32_t h = hle_arg(ctx, 0);
    uint32_t pprev = hle_arg(ctx, 1);
    ThreadControl* tc = nullptr;
    std::shared_ptr<ThreadControl> owned_tc;
    int32_t prev = 0;
    {
        std::lock_guard<std::mutex> lk(g_diag_mtx);
        if (h == 0xFFFFFFFEu) tc = tl_self_ctrl;
        else if (auto it = g_thr_by_handle.find(h); it != g_thr_by_handle.end()) {
            owned_tc = it->second;
            tc = owned_tc.get();
        }
        if (tc && !g_krnl_shutting_down.load(std::memory_order_acquire)) {
            prev = tc->suspend_cnt.load(std::memory_order_relaxed);
            tc->suspend_cnt.store(prev + 1, std::memory_order_release);
            host_reset_event(tc->resume_ev);
        }
    }
    if (pprev) wr32(pprev, static_cast<uint32_t>(prev));
    record_hle_op(ctx, "NtSuspendThread", h, static_cast<uint32_t>(prev));
    if (tc && tc == tl_self_ctrl) {
        wait_if_self_suspended();
    }
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 8);
}

static void hle_NtResumeThread(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    uint32_t pprev = hle_arg(ctx, 1);
    int32_t prev = 0;
    {
        std::lock_guard<std::mutex> lk(g_diag_mtx);
        ThreadControl* tc = nullptr;
        std::shared_ptr<ThreadControl> owned_tc;
        if (h == 0xFFFFFFFEu) tc = tl_self_ctrl;
        else if (auto it = g_thr_by_handle.find(h); it != g_thr_by_handle.end()) {
            owned_tc = it->second;
            tc = owned_tc.get();
        }
        if (tc) {
            prev = tc->suspend_cnt.load(std::memory_order_relaxed);
            if (prev > 0) {
                int32_t next = prev - 1;
                tc->suspend_cnt.store(next, std::memory_order_release);
                if (next == 0) host_set_event(tc->resume_ev);
            }
        }
    }
    if (pprev) wr32(pprev, static_cast<uint32_t>(prev));
    record_hle_op(ctx, "NtResumeThread", h, static_cast<uint32_t>(prev));
    hle_ret_u32(ctx, STATUS_SUCCESS_XB, 8);
}
DEF_STUB(NtSetSystemTime, 0, 8)
DEF_STUB(FscGetCacheSize, 4096, 4)
DEF_STUB(FscSetCacheSize, 0, 4)
DEF_STUB(HalIsResetOrShutdownPending, 0, 0)
DEF_VOID_STUB(HalInitiateShutdown, 0)
DEF_VOID_STUB(HalRegisterShutdownNotification, 8)
DEF_VOID_STUB(HalReadWritePCISpace, 24)
DEF_STUB(HalGetInterruptVector, 0, 8)
DEF_STUB(IoCreateDevice, 0, 24)
DEF_STUB(IoInvalidDeviceRequest, 0, 8)
DEF_STUB(IoBuildSynchronousFsdRequest, 0, 28)
DEF_STUB(IoSynchronousFsdRequest, 0, 20)
DEF_STUB(IoSynchronousDeviceIoControlRequest, 0, 32)
DEF_STUB(IoDismountVolumeByName, 0, 4)
DEF_VOID_STUB(IoStartPacket, 16)
DEF_VOID_STUB(IoStartNextPacket, 8)
DEF_VOID_STUB(IoMarkIrpMustComplete, 4)
DEF_STUB(IofCallDriver, 0, 4) // fastcall!
DEF_VOID_STUB(IofCompleteRequest, 4) // fastcall!
DEF_STUB(PhyInitialize, 0, 8)
DEF_STUB(PhyGetLinkState, 0, 4)
DEF_STUB(XeLoadSection, 0, 4)
DEF_STUB(XeUnloadSection, 0, 4)
DEF_VOID_STUB(RtlAnsiStringToUnicodeString, 12)
DEF_VOID_STUB(RtlUnicodeStringToAnsiString, 12)
DEF_VOID_STUB(RtlFreeAnsiString, 4)
DEF_VOID_STUB(RtlRaiseException, 4)
DEF_VOID_STUB(RtlUnwind, 16)
DEF_VOID_STUB(DbgBreakPoint, 0)
DEF_VOID_STUB(XcRC4Key, 12)
DEF_VOID_STUB(XcRC4Crypt, 12)
DEF_VOID_STUB(XcDESKeyParity, 8)
DEF_VOID_STUB(XcKeyTable, 12)
DEF_VOID_STUB(XcBlockCryptCBC, 20)
DEF_STUB(XcPKGetKeyLen, 256, 4)
DEF_STUB(XcVerifyPKCS1Signature, 1, 12)
DEF_STUB(XcModExp, 1, 16)
DEF_VOID_STUB(NtUserIoApcDispatcher, 12)
DEF_STUB(KeSetBasePriorityThread, 0, 8)
DEF_STUB(KeQueryBasePriorityThread, 0, 4)
DEF_STUB(KeSetDisableBoostThread, 0, 8)

struct OrdReg { uint32_t ord; RecompFn fn; bool is_data; };
} // namespace

void hle_kernel_update_ticks() {
    if (KD_TICKCOUNT_VA) {
        wr32(KD_TICKCOUNT_VA, static_cast<uint32_t>(host_ticks_ms()));
    }
}

void hle_kernel_signal_handle(uint32_t handle) {
    if (HostSync s = get_handle_sync(handle)) {
        host_set_event(s);
    }
}

void hle_kernel_request_shutdown() {
    g_krnl_shutting_down.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> dlk(g_diag_mtx);
        for (auto& kv : g_thr_by_handle) {
            if (kv.second) {
                kv.second->suspend_cnt.store(0, std::memory_order_release);
                host_set_event(kv.second->resume_ev);
            }
        }
    }
#ifndef _WIN32
    g_sync_cv.notify_all();
#endif
}

bool hle_kernel_is_shutting_down() {
    return g_krnl_shutting_down.load(std::memory_order_acquire);
}

bool hle_kernel_wait_for_guest_threads(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lk(g_diag_mtx);
    return g_thr_exit_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), []() {
        return g_guest_threads.empty();
    });
}

void hle_kernel_shutdown() {
    hle_kernel_request_shutdown();
    g_tick_running = false;
#ifndef _WIN32
    g_sync_cv.notify_all();
#endif
    if (g_tick_thread.joinable()) g_tick_thread.join();
}

void hle_register_kernel() {
    g_krnl_shutting_down.store(false, std::memory_order_release);
    constexpr uint32_t EBASE = HleKernelDataLayout::KERNEL_EXPORTS_BASE;
    std::memset(mem_ptr(EBASE), 0, 0x1000u);

    KD_TICKCOUNT_VA = EBASE + 0x100u;
    hle_kernel_update_ticks();

    // LaunchDataPage = NULL at EBASE + 0x104 -> leave 0
    // XboxHardwareInfo at EBASE + 0x120
    wr32(EBASE + 0x120u, 0x00000000u); // Flags (Retail)
    wr8 (EBASE + 0x124u, 0xD2);
    wr8 (EBASE + 0x125u, 0xD4);
    // XboxKrnlVersion at EBASE + 0x140: 1.0.4831.1
    wr16(EBASE + 0x140u, 1); wr16(EBASE + 0x142u, 0);
    wr16(EBASE + 0x144u, 4831); wr16(EBASE + 0x146u, 1);
    // XeImageFileName ANSI_STRING at EBASE + 0x160
    const char* img_name = "\\Device\\Cdrom0\\default.xbe";
    xbox_write_cstr(EBASE + 0x200u, img_name, 64);
    wr16(EBASE + 0x160u, static_cast<uint16_t>(std::strlen(img_name)));
    wr16(EBASE + 0x162u, 64);
    wr32(EBASE + 0x164u, EBASE + 0x200u);
    // Keys at EBASE + 0x280..
    for (uint32_t i = 0; i < 16; ++i) {
        wr8(EBASE + 0x280u + i, static_cast<uint8_t>(0x10 + i));
        wr8(EBASE + 0x290u + i, static_cast<uint8_t>(0x30 + i));
        wr8(EBASE + 0x2A0u + i, static_cast<uint8_t>(0x50 + i));
    }
    wr32(EBASE + 0x2C0u, 1);        // HalBootSMCVideoMode
    wr32(EBASE + 0x2C4u, 1);        // HalDiskCachePartitionCount
    wr32(EBASE + 0x2C8u, 10000);    // KeTimeIncrement (1ms)
    xbox_write_cstr(EBASE + 0x300u, "RECOMP000001", 32);
    xbox_write_cstr(EBASE + 0x340u, "XBOX-RECOMP-HDD", 32);

    std::unordered_map<uint32_t, uint32_t> data_exports = {
        {156, KD_TICKCOUNT_VA},        // KeTickCount
        {157, EBASE + 0x2C8u},         // KeTimeIncrement
        {164, EBASE + 0x104u},         // &LaunchDataPage (*LaunchDataPage == NULL)
        {322, EBASE + 0x120u},         // XboxHardwareInfo
        {323, EBASE + 0x280u},         // XboxHDKey
        {324, EBASE + 0x140u},         // XboxKrnlVersion
        {325, EBASE + 0x290u},         // XboxSignatureKey
        {326, EBASE + 0x160u},         // XeImageFileName
        {353, EBASE + 0x2A0u},         // XboxLANKey
        {354, EBASE + 0x2B0u},         // XboxAlternateSignatureKeys
        {355, EBASE + 0x380u},         // XePublicKeyData
        {356, EBASE + 0x2C0u},         // HalBootSMCVideoMode
        { 40, EBASE + 0x2C4u},         // HalDiskCachePartitionCount
        { 41, EBASE + 0x340u},         // HalDiskModelNumber
        { 42, EBASE + 0x300u},         // HalDiskSerialNumber
        { 16, EBASE + 0x400u},         // ExEventObjectType
        { 22, EBASE + 0x410u},         // ExMutantObjectType
        { 30, EBASE + 0x420u},         // ExSemaphoreObjectType
        { 31, EBASE + 0x430u},         // ExTimerObjectType
        { 71, EBASE + 0x440u},         // IoFileObjectType
        {259, EBASE + 0x450u},         // PsThreadObjectType
        {357, EBASE + 0x460u},         // IdexChannelObject
    };

    const OrdReg fn_regs[] = {
        {  1, hle_AvGetSavedDataAddress, false},
        {  2, hle_AvSendTVEncoderOption, false},
        {  3, hle_AvSetDisplayMode, false},
        {  4, hle_AvSetSavedDataAddress, false},
        {  5, hle_DbgBreakPoint, false},
        {  8, hle_DbgPrint, false},
        { 14, hle_ExAllocatePool, false},
        { 15, hle_ExAllocatePoolWithTag, false},
        { 17, hle_ExFreePool, false},
        { 23, hle_ExQueryPoolBlockSize, false},
        { 24, hle_ExQueryNonVolatileSetting, false},
        { 35, hle_FscGetCacheSize, false},
        { 37, hle_FscSetCacheSize, false},
        { 44, hle_HalGetInterruptVector, false},
        { 46, hle_HalReadWritePCISpace, false},
        { 47, hle_HalRegisterShutdownNotification, false},
        { 49, hle_HalReturnToFirmware, false},
        { 62, hle_IoBuildSynchronousFsdRequest, false},
        { 65, hle_IoCreateDevice, false},
        { 67, hle_IoCreateSymbolicLink, false},
        { 69, hle_IoDeleteSymbolicLink, false},
        { 74, hle_IoInvalidDeviceRequest, false},
        { 81, hle_IoStartNextPacket, false},
        { 83, hle_IoStartPacket, false},
        { 84, hle_IoSynchronousDeviceIoControlRequest, false},
        { 85, hle_IoSynchronousFsdRequest, false},
        { 86, hle_IofCallDriver, false},
        { 87, hle_IofCompleteRequest, false},
        { 91, hle_IoDismountVolumeByName, false},
        { 95, hle_KeBugCheck, false},
        { 97, hle_KeCancelTimer, false},
        { 98, hle_KeConnectInterrupt, false},
        { 99, hle_KeDelayExecutionThread, false},
        {100, hle_KeDisconnectInterrupt, false},
        {107, hle_KeInitializeDpc, false},
        {109, hle_KeInitializeInterrupt, false},
        {113, hle_KeInitializeTimerEx, false},
        {119, hle_KeInsertQueueDpc, false},
        {124, hle_KeQueryBasePriorityThread, false},
        {125, hle_KeQueryInterruptTime, false},
        {126, hle_KeQueryPerformanceCounter, false},
        {127, hle_KeQueryPerformanceFrequency, false},
        {128, hle_KeQuerySystemTime, false},
        {129, hle_KeRaiseIrqlToDpcLevel, false},
        {137, hle_KeRemoveQueueDpc, false},
        {139, hle_KeRestoreFloatingPointState, false},
        {142, hle_KeSaveFloatingPointState, false},
        {143, hle_KeSetBasePriorityThread, false},
        {144, hle_KeSetDisableBoostThread, false},
        {145, hle_KeSetEvent, false},
        {149, hle_KeSetTimer, false},
        {150, hle_KeSetTimerEx, false},
        {151, hle_KeStallExecutionProcessor, false},
        {153, hle_KeSynchronizeExecution, false},
        {158, hle_KeWaitForMultipleObjects, false},
        {159, hle_KeWaitForSingleObject, false},
        {160, hle_KfRaiseIrql, false},
        {161, hle_KfLowerIrql, false},
        {165, hle_MmAllocateContiguousMemory, false},
        {166, hle_MmAllocateContiguousMemoryEx, false},
        {167, hle_MmAllocateSystemMemory, false},
        {168, hle_MmClaimGpuInstanceMemory, false},
        {171, hle_MmFreeContiguousMemory, false},
        {172, hle_MmFreeSystemMemory, false},
        {173, hle_MmGetPhysicalAddress, false},
        {175, hle_MmLockUnlockBufferPages, false},
        {176, hle_MmLockUnlockPhysicalPage, false},
        {178, hle_MmPersistContiguousMemory, false},
        {179, hle_MmQueryAddressProtect, false},
        {180, hle_MmQueryAllocationSize, false},
        {181, hle_MmQueryStatistics, false},
        {182, hle_MmSetAddressProtect, false},
        {184, hle_NtAllocateVirtualMemory, false},
        {185, hle_NtCancelTimer, false},
        {186, hle_NtClearEvent, false},
        {187, hle_NtClose, false},
        {189, hle_NtCreateEvent, false},
        {190, hle_NtCreateFile, false},
        {191, hle_NtCreateIoCompletion, false},
        {192, hle_NtCreateMutant, false},
        {193, hle_NtCreateSemaphore, false},
        {194, hle_NtCreateTimer, false},
        {195, hle_NtDeleteFile, false},
        {196, hle_NtDeviceIoControlFile, false},
        {197, hle_NtDuplicateObject, false},
        {198, hle_NtFlushBuffersFile, false},
        {199, hle_NtFreeVirtualMemory, false},
        {200, hle_NtFsControlFile, false},
        {202, hle_NtOpenFile, false},
        {203, hle_NtOpenSymbolicLinkObject, false},
        {204, hle_NtProtectVirtualMemory, false},
        {205, hle_NtPulseEvent, false},
        {206, hle_NtQueueApcThread, false},
        {207, hle_NtQueryDirectoryFile, false},
        {210, hle_NtQueryFullAttributesFile, false},
        {211, hle_NtQueryInformationFile, false},
        {215, hle_NtQuerySymbolicLinkObject, false},
        {217, hle_NtQueryVirtualMemory, false},
        {218, hle_NtQueryVolumeInformationFile, false},
        {219, hle_NtReadFile, false},
        {220, hle_NtReadFileScatter, false},
        {221, hle_NtReleaseMutant, false},
        {222, hle_NtReleaseSemaphore, false},
        {223, hle_NtRemoveIoCompletion, false},
        {224, hle_NtResumeThread, false},
        {225, hle_NtSetEvent, false},
        {226, hle_NtSetInformationFile, false},
        {227, hle_NtSetIoCompletion, false},
        {228, hle_NtSetSystemTime, false},
        {229, hle_NtSetTimerEx, false},
        {230, hle_NtSignalAndWaitForSingleObjectEx, false},
        {231, hle_NtSuspendThread, false},
        {232, hle_NtUserIoApcDispatcher, false},
        {233, hle_NtWaitForSingleObject, false},
        {234, hle_NtWaitForSingleObjectEx, false},
        {235, hle_NtWaitForMultipleObjectsEx, false},
        {236, hle_NtWriteFile, false},
        {237, hle_NtWriteFileGather, false},
        {238, hle_NtYieldExecution, false},
        {243, hle_ObOpenObjectByName, false},
        {246, hle_ObReferenceObjectByHandle, false},
        {247, hle_ObReferenceObjectByName, false},
        {250, hle_ObfDereferenceObject, false},
        {252, hle_PhyGetLinkState, false},
        {253, hle_PhyInitialize, false},
        {255, hle_PsCreateSystemThreadEx, false},
        {258, hle_PsTerminateSystemThread, false},
        {260, hle_RtlAnsiStringToUnicodeString, false},
        {268, hle_RtlCompareMemory, false},
        {269, hle_RtlCompareMemoryUlong, false},
        {270, hle_RtlCompareString, false},
        {277, hle_RtlEnterCriticalSection, false},
        {279, hle_RtlEqualString, false},
        {285, hle_RtlFillMemoryUlong, false},
        {286, hle_RtlFreeAnsiString, false},
        {289, hle_RtlInitAnsiString, false},
        {290, hle_RtlInitUnicodeString, false},
        {291, hle_RtlInitializeCriticalSection, false},
        {294, hle_RtlLeaveCriticalSection, false},
        {301, hle_RtlNtStatusToDosError, false},
        {302, hle_RtlRaiseException, false},
        {304, hle_RtlTimeFieldsToTime, false},
        {305, hle_RtlTimeToTimeFields, false},
        {308, hle_RtlUnicodeStringToAnsiString, false},
        {312, hle_RtlUnwind, false},
        {327, hle_XeLoadSection, false},
        {328, hle_XeUnloadSection, false},
        {335, hle_XcSHAInit, false},
        {336, hle_XcSHAUpdate, false},
        {337, hle_XcSHAFinal, false},
        {338, hle_XcRC4Key, false},
        {339, hle_XcRC4Crypt, false},
        {340, hle_XcHMAC, false},
        {343, hle_XcPKGetKeyLen, false},
        {344, hle_XcVerifyPKCS1Signature, false},
        {345, hle_XcModExp, false},
        {346, hle_XcDESKeyParity, false},
        {347, hle_XcKeyTable, false},
        {349, hle_XcBlockCryptCBC, false},
        {358, hle_HalIsResetOrShutdownPending, false},
        {359, hle_IoMarkIrpMustComplete, false},
        {360, hle_HalInitiateShutdown, false},
    };
    for (const auto& r : fn_regs) {
        recomp_register_fn(HleKernelDataLayout::KERNEL_STUB_BASE + r.ord, r.fn);
    }

    // Patch XBE kernel IAT table in guest RAM! (idempotent)
    uint32_t thunk_va = g_xbe_info.thunk_va ? g_xbe_info.thunk_va : 0x002C2EC0u;
    int patched = 0;
    for (uint32_t va = thunk_va; va < thunk_va + 0x1000u; va += 4u) {
        uint32_t raw = rd32(va);
        if (raw == 0) break;
        if ((raw & 0xFFFF0000u) == HleKernelDataLayout::KERNEL_STUB_BASE ||
            (raw >= EBASE && raw < EBASE + 0x1000u)) {
            continue;
        }
        uint32_t ord = (raw & 0x80000000u) ? (raw & 0xFFFFu) : ((raw ^ 0x5B6D40B6u) & 0xFFFFu);
        auto it = data_exports.find(ord);
        if (it != data_exports.end()) {
            wr32(va, it->second);
        } else {
            wr32(va, HleKernelDataLayout::KERNEL_STUB_BASE + ord);
        }
        ++patched;
    }

    // Start 1ms tick updater thread for KeTickCount
    if (!g_tick_running.exchange(true)) {
        g_tick_thread = std::thread([]() {
            while (g_tick_running.load()) {
                hle_kernel_update_ticks();
                host_sleep_ms(1);
            }
        });
    }

    std::printf("[HLE] Patched %d Xbox Kernel IAT slots at 0x%08X\n", patched, thunk_va);
}

#ifdef WHACKED_RUNTIME_TESTS
std::string test_kernel_resolve_attrs(uint32_t obj_attr_va) {
    return resolve_object_attrs(obj_attr_va);
}
void test_kernel_io_create_symlink(CpuContext& ctx) { hle_IoCreateSymbolicLink(ctx); }
void test_kernel_io_delete_symlink(CpuContext& ctx) { hle_IoDeleteSymbolicLink(ctx); }
void test_kernel_nt_create_file(CpuContext& ctx) { hle_NtCreateFile(ctx); }
void test_kernel_nt_close(CpuContext& ctx) { hle_NtClose(ctx); }
#endif
