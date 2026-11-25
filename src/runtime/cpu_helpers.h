#pragma once

#include "cpu_context.h"
#include <algorithm>
#include <chrono>

inline uint64_t cpu_rdtsc() {
    static const auto t0 = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - t0).count();
    // 733.33 MHz Xbox Pentium III cycle counter (~0.7333 cycles/ns)
    return static_cast<uint64_t>((ns * 733ull) / 1000ull + 1000000ull);
}

// Condition predicates (Intel x86 EFLAGS)
inline bool cond_o(const CpuContext& c)  { return c.of(); }
inline bool cond_no(const CpuContext& c) { return !c.of(); }
inline bool cond_b(const CpuContext& c)  { return c.cf(); }
inline bool cond_ae(const CpuContext& c) { return !c.cf(); }
inline bool cond_e(const CpuContext& c)  { return c.zf(); }
inline bool cond_ne(const CpuContext& c) { return !c.zf(); }
inline bool cond_be(const CpuContext& c) { return c.cf() || c.zf(); }
inline bool cond_a(const CpuContext& c)  { return !c.cf() && !c.zf(); }
inline bool cond_s(const CpuContext& c)  { return c.sf(); }
inline bool cond_ns(const CpuContext& c) { return !c.sf(); }
inline bool cond_p(const CpuContext& c)  { return c.pf(); }
inline bool cond_np(const CpuContext& c) { return !c.pf(); }
inline bool cond_l(const CpuContext& c)  { return c.sf() != c.of(); }
inline bool cond_ge(const CpuContext& c) { return c.sf() == c.of(); }
inline bool cond_le(const CpuContext& c) { return c.zf() || (c.sf() != c.of()); }
inline bool cond_g(const CpuContext& c)  { return !c.zf() && (c.sf() == c.of()); }

inline uint32_t alu_imul1_32(CpuContext& c, uint32_t src) {
    int64_t r = static_cast<int64_t>(static_cast<int32_t>(c.eax)) *
                static_cast<int64_t>(static_cast<int32_t>(src));
    c.eax = static_cast<uint32_t>(r);
    c.edx = static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32);
    bool ov = (r != static_cast<int64_t>(static_cast<int32_t>(c.eax)));
    c.set_flag(EFL_CF, ov);
    c.set_flag(EFL_OF, ov);
    return c.eax;
}

inline uint8_t alu_imul1_8(CpuContext& c, uint8_t src) {
    int16_t r = static_cast<int16_t>(static_cast<int8_t>(c.al())) *
                static_cast<int16_t>(static_cast<int8_t>(src));
    c.set_ax(static_cast<uint16_t>(r));
    bool ov = (r != static_cast<int16_t>(static_cast<int8_t>(c.al())));
    c.set_flag(EFL_CF, ov);
    c.set_flag(EFL_OF, ov);
    return c.al();
}

inline uint32_t alu_imul2_32(CpuContext& c, uint32_t a, uint32_t b) {
    int64_t r = static_cast<int64_t>(static_cast<int32_t>(a)) *
                static_cast<int64_t>(static_cast<int32_t>(b));
    uint32_t res = static_cast<uint32_t>(r);
    bool ov = (r != static_cast<int64_t>(static_cast<int32_t>(res)));
    c.set_flag(EFL_CF, ov);
    c.set_flag(EFL_OF, ov);
    return res;
}

inline uint16_t alu_imul2_16(CpuContext& c, uint16_t a, uint16_t b) {
    int32_t r = static_cast<int32_t>(static_cast<int16_t>(a)) *
                static_cast<int32_t>(static_cast<int16_t>(b));
    uint16_t res = static_cast<uint16_t>(r);
    bool ov = (r != static_cast<int32_t>(static_cast<int16_t>(res)));
    c.set_flag(EFL_CF, ov);
    c.set_flag(EFL_OF, ov);
    return res;
}

inline void alu_mul32(CpuContext& c, uint32_t src) {
    uint64_t r = static_cast<uint64_t>(c.eax) * static_cast<uint64_t>(src);
    c.eax = static_cast<uint32_t>(r);
    c.edx = static_cast<uint32_t>(r >> 32);
    bool ov = (c.edx != 0);
    c.set_flag(EFL_CF, ov);
    c.set_flag(EFL_OF, ov);
}

inline void alu_mul16(CpuContext& c, uint16_t src) {
    uint32_t r = static_cast<uint32_t>(c.ax()) * static_cast<uint32_t>(src);
    c.set_ax(static_cast<uint16_t>(r));
    c.set_dx(static_cast<uint16_t>(r >> 16));
    bool ov = (c.dx() != 0);
    c.set_flag(EFL_CF, ov);
    c.set_flag(EFL_OF, ov);
}

inline void alu_mul8(CpuContext& c, uint8_t src) {
    uint16_t r = static_cast<uint16_t>(c.al()) * static_cast<uint16_t>(src);
    c.set_ax(r);
    bool ov = (c.ah() != 0);
    c.set_flag(EFL_CF, ov);
    c.set_flag(EFL_OF, ov);
}

inline void alu_div32(CpuContext& c, uint32_t src) {
    if (src == 0) return;
    uint64_t num = (static_cast<uint64_t>(c.edx) << 32) | c.eax;
    c.eax = static_cast<uint32_t>(num / src);
    c.edx = static_cast<uint32_t>(num % src);
}

inline void alu_div16(CpuContext& c, uint16_t src) {
    if (src == 0) return;
    uint32_t num = (static_cast<uint32_t>(c.dx()) << 16) | c.ax();
    c.set_ax(static_cast<uint16_t>(num / src));
    c.set_dx(static_cast<uint16_t>(num % src));
}

inline void alu_idiv32(CpuContext& c, uint32_t src) {
    int32_t d = static_cast<int32_t>(src);
    if (d == 0) return;
    int64_t num = (static_cast<int64_t>(static_cast<int32_t>(c.edx)) << 32) | c.eax;
    c.eax = static_cast<uint32_t>(static_cast<int32_t>(num / d));
    c.edx = static_cast<uint32_t>(static_cast<int32_t>(num % d));
}

inline void alu_idiv16(CpuContext& c, uint16_t src) {
    int16_t d = static_cast<int16_t>(src);
    if (d == 0) return;
    int32_t num = (static_cast<int32_t>(static_cast<int16_t>(c.dx())) << 16) | c.ax();
    c.set_ax(static_cast<uint16_t>(static_cast<int16_t>(num / d)));
    c.set_dx(static_cast<uint16_t>(static_cast<int16_t>(num % d)));
}

inline double rd_ld80(uint32_t va) {
    const uint8_t* p = mem_ptr(va);
    uint64_t mant;
    uint16_t sign_exp;
    std::memcpy(&mant, p, 8);
    std::memcpy(&sign_exp, p + 8, 2);
    bool sign = (sign_exp & 0x8000u) != 0;
    int exp = sign_exp & 0x7FFFu;
    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7FFFu) {
        if ((mant & 0x7FFFFFFFFFFFFFFFull) == 0) return sign ? -INFINITY : INFINITY;
        return NAN;
    }
    int real_exp = exp - 16383;
    double val = std::ldexp(static_cast<double>(mant), real_exp - 63);
    return sign ? -val : val;
}

inline void wr_ld80(uint32_t va, double v) {
    uint8_t* p = mem_ptr(va);
    if (v == 0.0) {
        std::memset(p, 0, 10);
        if (std::signbit(v)) p[9] = 0x80;
        return;
    }
    if (std::isnan(v)) {
        uint64_t mant = 0xC000000000000000ull;
        uint16_t se = 0x7FFFu;
        std::memcpy(p, &mant, 8); std::memcpy(p + 8, &se, 2);
        return;
    }
    if (std::isinf(v)) {
        uint64_t mant = 0x8000000000000000ull;
        uint16_t se = (std::signbit(v) ? 0x8000u : 0u) | 0x7FFFu;
        std::memcpy(p, &mant, 8); std::memcpy(p + 8, &se, 2);
        return;
    }
    bool sign = v < 0.0;
    int exp = 0;
    double m = std::frexp(sign ? -v : v, &exp); // [0.5, 1.0), true exp = exp - 1
    uint64_t mant = static_cast<uint64_t>(std::ldexp(m, 64));
    uint16_t se = static_cast<uint16_t>((sign ? 0x8000u : 0u) | ((exp - 1 + 16383) & 0x7FFFu));
    std::memcpy(p, &mant, 8);
    std::memcpy(p + 8, &se, 2);
}

// Bit scan / test helpers
inline uint32_t alu_bsf32(CpuContext& c, uint32_t dst, uint32_t src) {
    if (src == 0) { c.set_flag(EFL_ZF, true); return dst; }
    c.set_flag(EFL_ZF, false);
    uint32_t i = 0;
    while (((src >> i) & 1u) == 0) ++i;
    return i;
}

// MMX helpers
inline uint64_t mmx_pmaddwd(uint64_t a, uint64_t b) {
    int16_t sa[4], sb[4];
    std::memcpy(sa, &a, 8); std::memcpy(sb, &b, 8);
    int32_t r0 = int32_t(sa[0]) * sb[0] + int32_t(sa[1]) * sb[1];
    int32_t r1 = int32_t(sa[2]) * sb[2] + int32_t(sa[3]) * sb[3];
    uint64_t out;
    std::memcpy(&out, &r0, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(&out) + 4, &r1, 4);
    return out;
}
inline uint64_t mmx_packssdw(uint64_t a, uint64_t b) {
    int32_t ia[2], ib[2]; int16_t o[4];
    std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    auto sat = [](int32_t v) -> int16_t { return v > 32767 ? 32767 : (v < -32768 ? -32768 : int16_t(v)); };
    o[0] = sat(ia[0]); o[1] = sat(ia[1]); o[2] = sat(ib[0]); o[3] = sat(ib[1]);
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_packsswb(uint64_t a, uint64_t b) {
    int16_t ia[4], ib[4]; int8_t o[8];
    std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    auto sat = [](int16_t v) -> int8_t { return v > 127 ? 127 : (v < -128 ? -128 : int8_t(v)); };
    for (int i = 0; i < 4; ++i) { o[i] = sat(ia[i]); o[i+4] = sat(ib[i]); }
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_paddd(uint64_t a, uint64_t b) {
    int32_t ia[2], ib[2], o[2]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    o[0] = ia[0] + ib[0]; o[1] = ia[1] + ib[1];
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_psubd(uint64_t a, uint64_t b) {
    int32_t ia[2], ib[2], o[2]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    o[0] = ia[0] - ib[0]; o[1] = ia[1] - ib[1];
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_psubw(uint64_t a, uint64_t b) {
    int16_t ia[4], ib[4], o[4]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 4; ++i) o[i] = ia[i] - ib[i];
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_pcmpeqd(uint64_t a, uint64_t b) {
    uint32_t ia[2], ib[2], o[2]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    o[0] = (ia[0] == ib[0]) ? 0xFFFFFFFFu : 0u;
    o[1] = (ia[1] == ib[1]) ? 0xFFFFFFFFu : 0u;
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_pcmpeqw(uint64_t a, uint64_t b) {
    uint16_t ia[4], ib[4], o[4]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 4; ++i) o[i] = (ia[i] == ib[i]) ? 0xFFFFu : 0u;
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_pmaxsw(uint64_t a, uint64_t b) {
    int16_t ia[4], ib[4], o[4]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 4; ++i) o[i] = std::max(ia[i], ib[i]);
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_pminsw(uint64_t a, uint64_t b) {
    int16_t ia[4], ib[4], o[4]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 4; ++i) o[i] = std::min(ia[i], ib[i]);
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint32_t mmx_pmovmskb(uint64_t a) {
    uint32_t mask = 0;
    for (int i = 0; i < 8; ++i) if ((a >> (i * 8 + 7)) & 1ull) mask |= (1u << i);
    return mask;
}
inline uint64_t mmx_pshufw(uint64_t a, uint8_t imm) {
    uint16_t ia[4], o[4]; std::memcpy(ia, &a, 8);
    for (int i = 0; i < 4; ++i) o[i] = ia[(imm >> (i * 2)) & 3];
    uint64_t out; std::memcpy(&out, o, 8); return out;
}

// --- Byte/word saturated and wrapping MMX ops (needed by XOnline/XNet/Bink code) ---
inline uint64_t mmx_paddw(uint64_t a, uint64_t b) {
    uint16_t ia[4], ib[4], o[4]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 4; ++i) o[i] = static_cast<uint16_t>(ia[i] + ib[i]);
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_paddsw(uint64_t a, uint64_t b) {
    int16_t ia[4], ib[4]; int32_t t[4];
    std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 4; ++i) {
        t[i] = int32_t(ia[i]) + ib[i];
        t[i] = t[i] > 32767 ? 32767 : (t[i] < -32768 ? -32768 : t[i]);
    }
    uint64_t out; std::memcpy(&out, t, 8); return out;
}
inline uint64_t mmx_psubsw(uint64_t a, uint64_t b) {
    int16_t ia[4], ib[4]; int32_t t[4];
    std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 4; ++i) {
        t[i] = int32_t(ia[i]) - ib[i];
        t[i] = t[i] > 32767 ? 32767 : (t[i] < -32768 ? -32768 : t[i]);
    }
    uint64_t out; std::memcpy(&out, t, 8); return out;
}
inline uint64_t mmx_psubusw(uint64_t a, uint64_t b) {
    uint16_t ia[4], ib[4], o[4]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 4; ++i) o[i] = ia[i] > ib[i] ? static_cast<uint16_t>(ia[i] - ib[i]) : 0u;
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_pmulhw(uint64_t a, uint64_t b) {
    int16_t ia[4], ib[4], o[4]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 4; ++i) o[i] = static_cast<int16_t>((int32_t(ia[i]) * ib[i]) >> 16);
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_pmullw(uint64_t a, uint64_t b) {
    int16_t ia[4], ib[4]; uint16_t o[4]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 4; ++i) o[i] = static_cast<uint16_t>(int32_t(ia[i]) * ib[i]);
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_punpcklbw(uint64_t a, uint64_t b) {
    uint8_t ia[8], ib[8], o[8]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 4; ++i) { o[i * 2] = ia[i]; o[i * 2 + 1] = ib[i]; }
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_punpckhbw(uint64_t a, uint64_t b) {
    uint8_t ia[8], ib[8], o[8]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 4; ++i) { o[i * 2] = ia[i + 4]; o[i * 2 + 1] = ib[i + 4]; }
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_punpckhwd(uint64_t a, uint64_t b) {
    uint16_t ia[4], ib[4], o[4]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    o[0] = ia[2]; o[1] = ib[2]; o[2] = ia[3]; o[3] = ib[3];
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_paddb(uint64_t a, uint64_t b) {
    uint8_t ia[8], ib[8], o[8]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 8; ++i) o[i] = static_cast<uint8_t>(ia[i] + ib[i]);
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_psubb(uint64_t a, uint64_t b) {
    uint8_t ia[8], ib[8], o[8]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 8; ++i) o[i] = static_cast<uint8_t>(ia[i] - ib[i]);
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_paddsb(uint64_t a, uint64_t b) {
    int8_t ia[8], ib[8]; int16_t t[8];
    std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 8; ++i) {
        t[i] = int16_t(ia[i]) + ib[i];
        t[i] = t[i] > 127 ? 127 : (t[i] < -128 ? -128 : t[i]);
    }
    int8_t o[8]; for (int i = 0; i < 8; ++i) o[i] = static_cast<int8_t>(t[i]);
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_psubsb(uint64_t a, uint64_t b) {
    int8_t ia[8], ib[8]; int16_t t[8];
    std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 8; ++i) {
        t[i] = int16_t(ia[i]) - ib[i];
        t[i] = t[i] > 127 ? 127 : (t[i] < -128 ? -128 : t[i]);
    }
    int8_t o[8]; for (int i = 0; i < 8; ++i) o[i] = static_cast<int8_t>(t[i]);
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_psubusb(uint64_t a, uint64_t b) {
    uint8_t ia[8], ib[8], o[8]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 8; ++i) o[i] = ia[i] > ib[i] ? static_cast<uint8_t>(ia[i] - ib[i]) : 0u;
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_pandn(uint64_t a, uint64_t b) { return (~a) & b; }
inline uint64_t mmx_pavgb(uint64_t a, uint64_t b) {
    uint8_t ia[8], ib[8], o[8]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 8; ++i) o[i] = static_cast<uint8_t>((uint32_t(ia[i]) + ib[i] + 1) >> 1);
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_pavgw(uint64_t a, uint64_t b) {
    uint16_t ia[4], ib[4], o[4]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 4; ++i) o[i] = static_cast<uint16_t>((uint32_t(ia[i]) + ib[i] + 1) >> 1);
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_pcmpeqb(uint64_t a, uint64_t b) {
    uint8_t ia[8], ib[8], o[8]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 8; ++i) o[i] = (ia[i] == ib[i]) ? 0xFFu : 0u;
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_pcmpgtb(uint64_t a, uint64_t b) {
    int8_t ia[8], ib[8], o[8]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 8; ++i) o[i] = (ia[i] > ib[i]) ? -1 : 0;
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_pcmpgtw(uint64_t a, uint64_t b) {
    int16_t ia[4], ib[4], o[4]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    for (int i = 0; i < 4; ++i) o[i] = (ia[i] > ib[i]) ? -1 : 0;
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_pcmpgtd(uint64_t a, uint64_t b) {
    int32_t ia[2], ib[2], o[2]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    o[0] = (ia[0] > ib[0]) ? -1 : 0; o[1] = (ia[1] > ib[1]) ? -1 : 0;
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_packuswb(uint64_t a, uint64_t b) {
    int16_t ia[4], ib[4]; uint8_t o[8];
    std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    auto sat = [](int16_t v) -> uint8_t { return v > 255 ? 255u : (v < 0 ? 0u : uint8_t(v)); };
    for (int i = 0; i < 4; ++i) { o[i] = sat(ia[i]); o[i + 4] = sat(ib[i]); }
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_punpcklqdq(uint64_t a, uint64_t b) { (void)b; return a; }
inline uint64_t mmx_punpcklwd(uint64_t a, uint64_t b) {
    uint16_t ia[4], ib[4], o[4]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    o[0] = ia[0]; o[1] = ib[0]; o[2] = ia[1]; o[3] = ib[1];
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_punpckldq(uint64_t a, uint64_t b) {
    uint32_t ia[2], ib[2], o[2]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    o[0] = ia[0]; o[1] = ib[0];
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_punpckhdq(uint64_t a, uint64_t b) {
    uint32_t ia[2], ib[2], o[2]; std::memcpy(ia, &a, 8); std::memcpy(ib, &b, 8);
    o[0] = ia[1]; o[1] = ib[1];
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_psllw(uint64_t a, uint32_t cnt) {
    if (cnt >= 16) return 0;
    uint16_t ia[4], o[4]; std::memcpy(ia, &a, 8);
    for (int i = 0; i < 4; ++i) o[i] = static_cast<uint16_t>(ia[i] << cnt);
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_psrlw(uint64_t a, uint32_t cnt) {
    if (cnt >= 16) return 0;
    uint16_t ia[4], o[4]; std::memcpy(ia, &a, 8);
    for (int i = 0; i < 4; ++i) o[i] = static_cast<uint16_t>(ia[i] >> cnt);
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_pslld(uint64_t a, uint32_t cnt) {
    if (cnt >= 32) return 0;
    uint32_t ia[2], o[2]; std::memcpy(ia, &a, 8);
    o[0] = ia[0] << cnt; o[1] = ia[1] << cnt;
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_psrld(uint64_t a, uint32_t cnt) {
    if (cnt >= 32) return 0;
    uint32_t ia[2], o[2]; std::memcpy(ia, &a, 8);
    o[0] = ia[0] >> cnt; o[1] = ia[1] >> cnt;
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_psrad(uint64_t a, uint32_t cnt) {
    uint32_t sh = cnt >= 32 ? 31 : cnt;
    int32_t ia[2], o[2]; std::memcpy(ia, &a, 8);
    o[0] = ia[0] >> sh; o[1] = ia[1] >> sh;
    uint64_t out; std::memcpy(&out, o, 8); return out;
}
inline uint64_t mmx_pinsrw(uint64_t a, uint16_t w, uint8_t idx) {
    uint16_t ia[4]; std::memcpy(ia, &a, 8);
    ia[idx & 3] = w;
    uint64_t out; std::memcpy(&out, ia, 8); return out;
}

inline XmmReg rd_xmm(uint32_t va) { XmmReg r; std::memcpy(&r, mem_ptr(va), 16); return r; }
inline void wr_xmm(uint32_t va, const XmmReg& r) { std::memcpy(mem_ptr(va), &r, 16); }
