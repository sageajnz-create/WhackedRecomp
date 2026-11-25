#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

extern uint8_t* g_xbox_mem_base;
constexpr uint32_t XBOX_MEM_SIZE = 0x10000000u; // 256 MiB flat guest address space

inline uint8_t* mem_ptr(uint32_t va) {
    return g_xbox_mem_base + va;
}

inline uint8_t rd8(uint32_t va) { return *reinterpret_cast<const uint8_t*>(mem_ptr(va)); }
inline uint16_t rd16(uint32_t va) { return *reinterpret_cast<const uint16_t*>(mem_ptr(va)); }
inline uint32_t rd32(uint32_t va) { return *reinterpret_cast<const uint32_t*>(mem_ptr(va)); }
inline uint64_t rd64(uint32_t va) { return *reinterpret_cast<const uint64_t*>(mem_ptr(va)); }
inline float rdf32(uint32_t va) {
    float f;
    std::memcpy(&f, mem_ptr(va), 4);
    return f;
}
inline double rdf64(uint32_t va) {
    double d;
    std::memcpy(&d, mem_ptr(va), 8);
    return d;
}

inline void wr8(uint32_t va, uint8_t v) { *reinterpret_cast<uint8_t*>(mem_ptr(va)) = v; }
inline void wr16(uint32_t va, uint16_t v) { *reinterpret_cast<uint16_t*>(mem_ptr(va)) = v; }
inline void wr32(uint32_t va, uint32_t v) { *reinterpret_cast<uint32_t*>(mem_ptr(va)) = v; }
inline void wr64(uint32_t va, uint64_t v) { *reinterpret_cast<uint64_t*>(mem_ptr(va)) = v; }
inline void wrf32(uint32_t va, float v) { std::memcpy(mem_ptr(va), &v, 4); }
inline void wrf64(uint32_t va, double v) { std::memcpy(mem_ptr(va), &v, 8); }

enum EFlagsBits : uint32_t {
    EFL_CF = 1u << 0,
    EFL_PF = 1u << 2,
    EFL_AF = 1u << 4,
    EFL_ZF = 1u << 6,
    EFL_SF = 1u << 7,
    EFL_DF = 1u << 10,
    EFL_OF = 1u << 11,
};

union XmmReg {
    float f[4];
    double d[2];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
    uint16_t w[8];
    uint8_t b[16];
};

struct CpuContext {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    uint32_t esi = 0, edi = 0, ebp = 0, esp = 0;
    uint32_t eflags = 0x202;
    uint32_t fs_base = 0;
    uint32_t last_pc = 0;

    // x87 FPU stack (st[fpu_top & 7] is ST(0))
    double st[8] = {};
    int fpu_top = 0;
    uint16_t fpu_cw = 0x037F;
    uint16_t fpu_sw = 0;

    // MMX / SSE registers (in x86 MM0..MM7 alias x87 mantissa; kept separate cleanly here since EMMS separates use)
    uint64_t mm[8] = {};
    XmmReg xmm[8] = {};

    // Sub-register accessors
    uint16_t ax() const { return static_cast<uint16_t>(eax); }
    uint16_t bx() const { return static_cast<uint16_t>(ebx); }
    uint16_t cx() const { return static_cast<uint16_t>(ecx); }
    uint16_t dx() const { return static_cast<uint16_t>(edx); }
    uint16_t si() const { return static_cast<uint16_t>(esi); }
    uint16_t di() const { return static_cast<uint16_t>(edi); }
    uint16_t bp() const { return static_cast<uint16_t>(ebp); }
    uint16_t sp() const { return static_cast<uint16_t>(esp); }

    void set_ax(uint16_t v) { eax = (eax & 0xFFFF0000u) | v; }
    void set_bx(uint16_t v) { ebx = (ebx & 0xFFFF0000u) | v; }
    void set_cx(uint16_t v) { ecx = (ecx & 0xFFFF0000u) | v; }
    void set_dx(uint16_t v) { edx = (edx & 0xFFFF0000u) | v; }
    void set_si(uint16_t v) { esi = (esi & 0xFFFF0000u) | v; }
    void set_di(uint16_t v) { edi = (edi & 0xFFFF0000u) | v; }
    void set_bp(uint16_t v) { ebp = (ebp & 0xFFFF0000u) | v; }
    void set_sp(uint16_t v) { esp = (esp & 0xFFFF0000u) | v; }

    uint8_t al() const { return static_cast<uint8_t>(eax); }
    uint8_t ah() const { return static_cast<uint8_t>(eax >> 8); }
    uint8_t bl() const { return static_cast<uint8_t>(ebx); }
    uint8_t bh() const { return static_cast<uint8_t>(ebx >> 8); }
    uint8_t cl() const { return static_cast<uint8_t>(ecx); }
    uint8_t ch() const { return static_cast<uint8_t>(ecx >> 8); }
    uint8_t dl() const { return static_cast<uint8_t>(edx); }
    uint8_t dh() const { return static_cast<uint8_t>(edx >> 8); }

    void set_al(uint8_t v) { eax = (eax & 0xFFFFFF00u) | v; }
    void set_ah(uint8_t v) { eax = (eax & 0xFFFF00FFu) | (uint32_t(v) << 8); }
    void set_bl(uint8_t v) { ebx = (ebx & 0xFFFFFF00u) | v; }
    void set_bh(uint8_t v) { ebx = (ebx & 0xFFFF00FFu) | (uint32_t(v) << 8); }
    void set_cl(uint8_t v) { ecx = (ecx & 0xFFFFFF00u) | v; }
    void set_ch(uint8_t v) { ecx = (ecx & 0xFFFF00FFu) | (uint32_t(v) << 8); }
    void set_dl(uint8_t v) { edx = (edx & 0xFFFFFF00u) | v; }
    void set_dh(uint8_t v) { edx = (edx & 0xFFFF00FFu) | (uint32_t(v) << 8); }

    // Stack helpers
    void push32(uint32_t v) { esp -= 4; wr32(esp, v); }
    uint32_t pop32() { uint32_t v = rd32(esp); esp += 4; return v; }
    void push16(uint16_t v) { esp -= 2; wr16(esp, v); }
    uint16_t pop16() { uint16_t v = rd16(esp); esp += 2; return v; }

    // Flag helpers
    bool cf() const { return (eflags & EFL_CF) != 0; }
    bool pf() const { return (eflags & EFL_PF) != 0; }
    bool zf() const { return (eflags & EFL_ZF) != 0; }
    bool sf() const { return (eflags & EFL_SF) != 0; }
    bool df() const { return (eflags & EFL_DF) != 0; }
    bool of() const { return (eflags & EFL_OF) != 0; }

    void set_flag(uint32_t bit, bool v) {
        if (v) eflags |= bit; else eflags &= ~bit;
    }

    // FPU helpers
    double& fpu_reg(int idx) { return st[(fpu_top + idx) & 7]; }
    double fpu_get(int idx) const { return st[(fpu_top + idx) & 7]; }
    void fpu_push(double v) {
        fpu_top = (fpu_top - 1) & 7;
        st[fpu_top] = v;
        fpu_sw = (fpu_sw & ~0x3800u) | uint16_t(fpu_top << 11);
    }
    double fpu_pop() {
        double v = st[fpu_top];
        fpu_top = (fpu_top + 1) & 7;
        fpu_sw = (fpu_sw & ~0x3800u) | uint16_t(fpu_top << 11);
        return v;
    }
    void fpu_set_cc(bool c0, bool c2, bool c3) {
        fpu_sw &= ~0x4500u;
        if (c0) fpu_sw |= 0x0100u;
        if (c2) fpu_sw |= 0x0400u;
        if (c3) fpu_sw |= 0x4000u;
    }
    void fpu_com(double a, double b) {
        if (std::isnan(a) || std::isnan(b)) { fpu_set_cc(true, true, true); }
        else if (a < b) { fpu_set_cc(true, false, false); }
        else if (a > b) { fpu_set_cc(false, false, false); }
        else { fpu_set_cc(false, false, true); }
    }
    void fpu_comi(double a, double b) {
        if (std::isnan(a) || std::isnan(b)) {
            set_flag(EFL_ZF, true); set_flag(EFL_PF, true); set_flag(EFL_CF, true);
        } else if (a < b) {
            set_flag(EFL_ZF, false); set_flag(EFL_PF, false); set_flag(EFL_CF, true);
        } else if (a > b) {
            set_flag(EFL_ZF, false); set_flag(EFL_PF, false); set_flag(EFL_CF, false);
        } else {
            set_flag(EFL_ZF, true); set_flag(EFL_PF, false); set_flag(EFL_CF, false);
        }
        set_flag(EFL_OF, false);
        set_flag(EFL_SF, false);
    }
    void fpu_fxam() {
        double v = st[fpu_top];
        uint64_t bits; std::memcpy(&bits, &v, 8);
        bool neg = (bits >> 63) != 0;
        fpu_sw &= ~0x4700u;
        if (neg) fpu_sw |= 0x0200u; // C1
        if (std::isnan(v)) fpu_sw |= 0x0100u; // C0
        else if (std::isinf(v)) fpu_sw |= 0x0500u; // C0|C2
        else if (v == 0.0) fpu_sw |= 0x4000u; // C3
        else fpu_sw |= 0x0400u; // C2 (normal)
    }
    double fpu_round_to_int(double v) const {
        int rc = (fpu_cw >> 10) & 3;
        switch (rc) {
            case 0: return std::nearbyint(v);
            case 1: return std::floor(v);
            case 2: return std::ceil(v);
            case 3: return std::trunc(v);
        }
        return std::nearbyint(v);
    }
};

inline bool parity_byte(uint8_t b) {
    uint8_t x = b ^ (b >> 4);
    x ^= (x >> 2);
    x ^= (x >> 1);
    return (x & 1) == 0;
}

template <typename T>
inline void update_szp(CpuContext& ctx, T res) {
    constexpr uint32_t bits = sizeof(T) * 8;
    ctx.set_flag(EFL_ZF, res == 0);
    ctx.set_flag(EFL_SF, (uint64_t(res) >> (bits - 1)) & 1);
    ctx.set_flag(EFL_PF, parity_byte(static_cast<uint8_t>(res)));
}

template <typename T>
inline T alu_add(CpuContext& ctx, T a, T b, bool carry_in = false) {
    using ST = std::make_signed_t<T>;
    uint64_t wide = uint64_t(a) + uint64_t(b) + (carry_in ? 1u : 0u);
    T res = static_cast<T>(wide);
    ctx.set_flag(EFL_CF, (wide >> (sizeof(T) * 8)) != 0);
    ST sa = static_cast<ST>(a), sb = static_cast<ST>(b), sr = static_cast<ST>(res);
    ctx.set_flag(EFL_OF, ((sa >= 0 && sb >= 0 && sr < 0) || (sa < 0 && sb < 0 && sr >= 0)));
    ctx.set_flag(EFL_AF, (((a ^ b ^ res) & 0x10) != 0));
    update_szp(ctx, res);
    return res;
}

template <typename T>
inline T alu_sub(CpuContext& ctx, T a, T b, bool borrow_in = false) {
    using ST = std::make_signed_t<T>;
    uint64_t wide_b = uint64_t(b) + (borrow_in ? 1u : 0u);
    T res = static_cast<T>(uint64_t(a) - wide_b);
    ctx.set_flag(EFL_CF, uint64_t(a) < wide_b);
    ST sa = static_cast<ST>(a), sb = static_cast<ST>(b), sr = static_cast<ST>(res);
    ctx.set_flag(EFL_OF, ((sa >= 0 && sb < 0 && sr < 0) || (sa < 0 && sb >= 0 && sr >= 0)));
    ctx.set_flag(EFL_AF, (((a ^ b ^ res) & 0x10) != 0));
    update_szp(ctx, res);
    return res;
}

template <typename T>
inline T alu_logic(CpuContext& ctx, T res) {
    ctx.set_flag(EFL_CF, false);
    ctx.set_flag(EFL_OF, false);
    update_szp(ctx, res);
    return res;
}

template <typename T>
inline T alu_inc(CpuContext& ctx, T a) {
    bool saved_cf = ctx.cf();
    T r = alu_add<T>(ctx, a, static_cast<T>(1), false);
    ctx.set_flag(EFL_CF, saved_cf);
    return r;
}

template <typename T>
inline T alu_dec(CpuContext& ctx, T a) {
    bool saved_cf = ctx.cf();
    T r = alu_sub<T>(ctx, a, static_cast<T>(1), false);
    ctx.set_flag(EFL_CF, saved_cf);
    return r;
}

template <typename T>
inline T alu_shl(CpuContext& ctx, T a, uint32_t count) {
    count &= 31u;
    if (count == 0) return a;
    constexpr uint32_t bits = sizeof(T) * 8;
    uint64_t shifted = (count <= 32) ? (uint64_t(a) << count) : 0;
    T res = static_cast<T>(shifted);
    bool new_cf = (count <= bits) ? (((uint64_t(a) >> (bits - count)) & 1u) != 0) : false;
    ctx.set_flag(EFL_CF, new_cf);
    if (count == 1) {
        bool msb = (uint64_t(res) >> (bits - 1)) & 1u;
        ctx.set_flag(EFL_OF, msb != new_cf);
    }
    update_szp(ctx, res);
    return res;
}

template <typename T>
inline T alu_shr(CpuContext& ctx, T a, uint32_t count) {
    count &= 31u;
    if (count == 0) return a;
    constexpr uint32_t bits = sizeof(T) * 8;
    T res = (count < bits) ? static_cast<T>(a >> count) : static_cast<T>(0);
    bool new_cf = (count <= bits) ? (((a >> (count - 1)) & 1u) != 0) : false;
    ctx.set_flag(EFL_CF, new_cf);
    if (count == 1) {
        ctx.set_flag(EFL_OF, ((a >> (bits - 1)) & 1u) != 0);
    }
    update_szp(ctx, res);
    return res;
}

template <typename T>
inline T alu_sar(CpuContext& ctx, T a, uint32_t count) {
    count &= 31u;
    if (count == 0) return a;
    using ST = std::make_signed_t<T>;
    constexpr uint32_t bits = sizeof(T) * 8;
    ST sa = static_cast<ST>(a);
    T res = (count < bits) ? static_cast<T>(sa >> count) : static_cast<T>(sa < 0 ? -1 : 0);
    bool new_cf = (count <= bits) ? (((sa >> (count - 1)) & 1) != 0) : (sa < 0);
    ctx.set_flag(EFL_CF, new_cf);
    if (count == 1) ctx.set_flag(EFL_OF, false);
    update_szp(ctx, res);
    return res;
}

template <typename T>
inline T alu_rol(CpuContext& ctx, T a, uint32_t count) {
    constexpr uint32_t bits = sizeof(T) * 8;
    count &= 31u;
    if (count == 0) return a;
    uint32_t c = count % bits;
    T res = (c == 0) ? a : static_cast<T>((a << c) | (a >> (bits - c)));
    bool cf_bit = (res & 1u) != 0;
    ctx.set_flag(EFL_CF, cf_bit);
    if (count == 1) ctx.set_flag(EFL_OF, (((res >> (bits - 1)) ^ uint64_t(cf_bit)) & 1u) != 0);
    return res;
}

template <typename T>
inline T alu_ror(CpuContext& ctx, T a, uint32_t count) {
    constexpr uint32_t bits = sizeof(T) * 8;
    count &= 31u;
    if (count == 0) return a;
    uint32_t c = count % bits;
    T res = (c == 0) ? a : static_cast<T>((a >> c) | (a << (bits - c)));
    bool msb = ((res >> (bits - 1)) & 1u) != 0;
    ctx.set_flag(EFL_CF, msb);
    if (count == 1) {
        bool msb2 = ((res >> (bits - 2)) & 1u) != 0;
        ctx.set_flag(EFL_OF, msb != msb2);
    }
    return res;
}

template <typename T>
inline T alu_rcr(CpuContext& ctx, T a, uint32_t count) {
    constexpr uint32_t bits = sizeof(T) * 8;
    count = (count & 31u) % (bits + 1);
    T val = a;
    bool c = ctx.cf();
    for (uint32_t i = 0; i < count; ++i) {
        bool new_c = (val & 1u) != 0;
        val = static_cast<T>((val >> 1) | (c ? (T(1) << (bits - 1)) : 0));
        c = new_c;
    }
    ctx.set_flag(EFL_CF, c);
    return val;
}

template <typename T>
inline T alu_rcl(CpuContext& ctx, T a, uint32_t count) {
    constexpr uint32_t bits = sizeof(T) * 8;
    count = (count & 31u) % (bits + 1);
    T val = a;
    bool c = ctx.cf();
    for (uint32_t i = 0; i < count; ++i) {
        bool new_c = ((val >> (bits - 1)) & 1u) != 0;
        val = static_cast<T>((val << 1) | (c ? 1u : 0u));
        c = new_c;
    }
    ctx.set_flag(EFL_CF, c);
    return val;
}

inline uint32_t alu_shld32(CpuContext& ctx, uint32_t dst, uint32_t src, uint32_t count) {
    count &= 31u;
    if (count == 0) return dst;
    bool c = ((dst >> (32 - count)) & 1u) != 0;
    uint32_t res = (dst << count) | (src >> (32 - count));
    ctx.set_flag(EFL_CF, c);
    update_szp(ctx, res);
    return res;
}

inline uint32_t alu_shrd32(CpuContext& ctx, uint32_t dst, uint32_t src, uint32_t count) {
    count &= 31u;
    if (count == 0) return dst;
    bool c = ((dst >> (count - 1)) & 1u) != 0;
    uint32_t res = (dst >> count) | (src << (32 - count));
    ctx.set_flag(EFL_CF, c);
    update_szp(ctx, res);
    return res;
}

using RecompFn = void(*)(CpuContext& ctx);

void recomp_dispatch_call(CpuContext& ctx, uint32_t target_va);
void recomp_register_fn(uint32_t va, RecompFn fn);
void recomp_register_all();

// Diagnostics: which XBE section owns a guest VA, and whether a VA has a lifted body.
const char* recomp_section_name(uint32_t va);
bool recomp_has_fn(uint32_t va);

// Helper for host runtime to invoke a guest function with cdecl/stdcall args
uint32_t recomp_invoke_cdecl(CpuContext& ctx, uint32_t va);
uint32_t recomp_invoke_stdcall(CpuContext& ctx, uint32_t va);
