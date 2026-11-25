#include "cpu_context.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

extern void hle_register_all();

namespace {
struct SecRange { uint32_t lo, hi; const char* name; };
// Every section of the loaded XBE, in address order (generated).
static const SecRange kSecRanges[] = {
    { 0x00012000u, 0x001f8f00u, ".text" },
    { 0x001f8f00u, 0x002214acu, "XONLINE" },
    { 0x002214c0u, 0x00235100u, "XNET" },
    { 0x00235100u, 0x0025299cu, "D3D" },
    { 0x00255e00u, 0x0025ce00u, "XGRPH" },
    { 0x00261460u, 0x002861d4u, "DSOUND" },
    { 0x00286420u, 0x002a1078u, "WMADEC" },
    { 0x002a1080u, 0x002b1920u, "BINK" },
    { 0x002b1920u, 0x002b2b84u, "BINK32" },
    { 0x002b2ba0u, 0x002b4118u, "BINK32A" },
    { 0x002b4120u, 0x002b543cu, "BINK16" },
    { 0x002b5440u, 0x002b69e0u, "BINK4444" },
    { 0x002b69e0u, 0x002b7b14u, "BINK5551" },
    { 0x002b7b20u, 0x002b87ccu, "BINKYUY2" },
    { 0x002b87e0u, 0x002b887cu, "BINKYUY2#2" },
    { 0x002b8880u, 0x002b8938u, "BINKYUY2#3" },
    { 0x002b8940u, 0x002b89c8u, "BINKYUY2#4" },
    { 0x002b89e0u, 0x002b8b10u, "BINK16MX" },
    { 0x002b8b20u, 0x002b9078u, "BINK16X2" },
    { 0x002b9080u, 0x002b9278u, "BINK16M" },
    { 0x002b9280u, 0x002b943cu, "BINK32MX" },
    { 0x002b9440u, 0x002b99ccu, "BINK32X2" },
    { 0x002b99e0u, 0x002b9b2cu, "BINK32M" },
    { 0x002b9b40u, 0x002c2eb0u, "XPP" },
    { 0x002c2ec0u, 0x002deb88u, ".rdata" },
    { 0x002deba0u, 0x0037c33cu, ".data" },
    { 0x003bb440u, 0x003bb4f0u, ".data1" },
    { 0x003bb520u, 0x003bdd8cu, "XON_RD" },
    { 0x003bdda0u, 0x003c4f0cu, "DOLBY" },
    { 0x003c4f20u, 0x003d4678u, "BINKDATA" },
    { 0x003d4680u, 0x003d6e80u, "$$XTIMAGE" },
    { 0x003d6e80u, 0x003db4f4u, "dspimage" },
};

const char* recomp_section_of(uint32_t va) {
    for (const auto& s : kSecRanges)
        if (va >= s.lo && va < s.hi) return s.name;
    return nullptr;
}

constexpr uint32_t kLutBase = 0x00010000u;
constexpr uint32_t kLutEnd  = 0x002D0000u;
RecompFn g_fn_lut[kLutEnd - kLutBase]{};

std::unordered_map<uint32_t, RecompFn>& fn_table() {
    static std::unordered_map<uint32_t, RecompFn> t;
    return t;
}
} // namespace

const char* recomp_section_name(uint32_t va) { return recomp_section_of(va); }

bool recomp_has_fn(uint32_t va) {
    uint32_t off = va - kLutBase;
    if (off < (kLutEnd - kLutBase)) return g_fn_lut[off] != nullptr;
    auto& tbl = fn_table();
    return tbl.find(va) != tbl.end();
}

void recomp_register_fn(uint32_t va, RecompFn fn) {
    uint32_t off = va - kLutBase;
    if (off < (kLutEnd - kLutBase)) g_fn_lut[off] = fn;
    fn_table()[va] = fn;
}

// Log the guest stack words that look like return addresses into a known function.
// Guest calls push a literal return VA, so a stack scan recovers an approximate
// backtrace even though the lifted code has no frame pointers.
static void dump_guest_backtrace(CpuContext& ctx, uint32_t depth_bytes) {
    std::fprintf(stderr, "        guest stack candidates (esp=0x%08X):\n", ctx.esp);
    int shown = 0;
    for (uint32_t off = 0; off < depth_bytes; off += 4) {
        uint32_t v = rd32(ctx.esp + off);
        const char* sec = recomp_section_of(v);
        if (!sec) continue;
        bool known = recomp_has_fn(v);
        std::fprintf(stderr, "          [esp+0x%03X] = 0x%08X  %-8s %s\n",
                     off, v, sec, known ? "(function entry)" : "(code, not an entry)");
        if (++shown >= 24) break;
    }
    if (!shown) std::fprintf(stderr, "          <none - stack not yet written>\n");
}

void recomp_dispatch_call(CpuContext& ctx, uint32_t target_va) {
    uint32_t off = target_va - kLutBase;
    if (off < (kLutEnd - kLutBase)) {
        RecompFn fn = g_fn_lut[off];
        if (fn) { fn(ctx); return; }
    } else {
        auto& tbl = fn_table();
        auto it = tbl.find(target_va);
        if (it != tbl.end()) { it->second(ctx); return; }
    }
    const char* sec = recomp_section_of(target_va);
    std::fprintf(stderr,
        "\n[FATAL] Unregistered call target 0x%08X (%s)\n"
        "        from last_pc=0x%08X (%s)  eax=0x%08X ecx=0x%08X edx=0x%08X ebx=0x%08X\n"
        "        esi=0x%08X edi=0x%08X ebp=0x%08X esp=0x%08X\n",
        target_va, sec ? sec : "outside every XBE section",
        ctx.last_pc, recomp_section_of(ctx.last_pc) ? recomp_section_of(ctx.last_pc) : "?",
        ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esi, ctx.edi, ctx.ebp, ctx.esp);
    dump_guest_backtrace(ctx, 0x300u);
    std::fflush(stderr);
    if (const char* mode = std::getenv("WHACKED_ON_UNKNOWN")) {
        if (std::strcmp(mode, "zero") == 0) { ctx.eax = 0; return; }
    }
    std::abort();
}

uint32_t recomp_invoke_cdecl(CpuContext& ctx, uint32_t va) {
    ctx.push32(0xDEAD0001u);
    recomp_dispatch_call(ctx, va);
    return ctx.eax;
}

uint32_t recomp_invoke_stdcall(CpuContext& ctx, uint32_t va) {
    ctx.push32(0xDEAD0002u);
    recomp_dispatch_call(ctx, va);
    return ctx.eax;
}

extern void recomp_register_shard_00();
extern void recomp_register_shard_01();
extern void recomp_register_shard_02();
extern void recomp_register_shard_03();
extern void recomp_register_shard_04();
extern void recomp_register_shard_05();
extern void recomp_register_shard_06();
extern void recomp_register_shard_07();
extern void recomp_register_shard_08();
extern void recomp_register_shard_09();
extern void recomp_register_shard_10();
extern void recomp_register_shard_11();
extern void recomp_register_shard_12();
extern void recomp_register_shard_13();
extern void recomp_register_shard_14();
extern void recomp_register_shard_15();
extern void recomp_register_shard_16();
extern void recomp_register_shard_17();
extern void recomp_register_shard_18();
extern void recomp_register_shard_19();
extern void recomp_register_shard_20();
extern void recomp_register_shard_21();
extern void recomp_register_shard_22();
extern void recomp_register_shard_23();
extern void recomp_register_shard_24();
extern void recomp_register_shard_25();
extern void recomp_register_shard_26();
extern void recomp_register_shard_27();
extern void recomp_register_shard_28();
extern void recomp_register_shard_29();
extern void recomp_register_shard_30();
extern void recomp_register_shard_31();

void recomp_register_all() {
    fn_table().reserve(16384);
    recomp_register_shard_00();
    recomp_register_shard_01();
    recomp_register_shard_02();
    recomp_register_shard_03();
    recomp_register_shard_04();
    recomp_register_shard_05();
    recomp_register_shard_06();
    recomp_register_shard_07();
    recomp_register_shard_08();
    recomp_register_shard_09();
    recomp_register_shard_10();
    recomp_register_shard_11();
    recomp_register_shard_12();
    recomp_register_shard_13();
    recomp_register_shard_14();
    recomp_register_shard_15();
    recomp_register_shard_16();
    recomp_register_shard_17();
    recomp_register_shard_18();
    recomp_register_shard_19();
    recomp_register_shard_20();
    recomp_register_shard_21();
    recomp_register_shard_22();
    recomp_register_shard_23();
    recomp_register_shard_24();
    recomp_register_shard_25();
    recomp_register_shard_26();
    recomp_register_shard_27();
    recomp_register_shard_28();
    recomp_register_shard_29();
    recomp_register_shard_30();
    recomp_register_shard_31();
    hle_register_all(); // HLE hooks override recompiled thunks
}
