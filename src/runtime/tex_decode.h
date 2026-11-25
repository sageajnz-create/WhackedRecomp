#pragma once

#include "gfx_backend.h"
#include "xbox_mem.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

inline uint32_t xb_unswizzle_index(uint32_t x, uint32_t y, uint32_t logw, uint32_t logh) {
    uint32_t idx = 0, bit = 1, bx = 0, by = 0;
    while (bx < logw || by < logh) {
        if (bx < logw) { if (x & (1u << bx)) idx |= bit; bit <<= 1; ++bx; }
        if (by < logh) { if (y & (1u << by)) idx |= bit; bit <<= 1; ++by; }
    }
    return idx;
}

inline bool xb_is_dxt_format(uint32_t f) {
    return f == 0x0Cu || f == 0x0Eu || f == 0x0Fu;
}

inline bool xb_is_linear_format(uint32_t f) {
    return f == 0x12u || f == 0x1Eu || f == 0x1Fu || f == 0x10u || f == 0x11u ||
           f == 0x16u || f == 0x17u || f == 0x1Du || f == 0x19u || f == 0x1Au;
}

inline uint32_t xb_tex_bpp(uint32_t f) {
    if (f == 0x06u || f == 0x07u || f == 0x12u || f == 0x1Eu || f == 0x1Fu) return 4u;
    if (f == 0x02u || f == 0x03u || f == 0x04u || f == 0x05u || f == 0x10u || f == 0x11u || f == 0x1Du || f == 0x1Au) return 2u;
    return 1u;
}

inline void xb_decode_mip_to_bgra(const uint8_t* src_mip, uint32_t mw, uint32_t mh, uint32_t fmt,
                                  uint32_t pitch_override, const uint32_t* pal,
                                  std::vector<uint32_t>& out_bgra, size_t& out_consumed) {
    uint32_t bpp = xb_tex_bpp(fmt);
    bool is_lin = xb_is_linear_format(fmt);
    uint32_t pitch = (is_lin && pitch_override) ? pitch_override : (mw * bpp);
    uint32_t lw = 0, lh = 0;
    while ((1u << lw) < mw) ++lw;
    while ((1u << lh) < mh) ++lh;

    out_bgra.resize(static_cast<size_t>(mw) * mh);
    for (uint32_t y = 0; y < mh; ++y) {
        for (uint32_t x = 0; x < mw; ++x) {
            const uint8_t* sp = is_lin
                ? (src_mip + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * bpp)
                : (src_mip + static_cast<size_t>(xb_unswizzle_index(x, y, lw, lh)) * bpp);
            uint32_t c = 0xFFFFFFFFu;
            if (bpp == 4u) {
                std::memcpy(&c, sp, 4);
                if (fmt == 0x07u || fmt == 0x1Eu || fmt == 0x1Fu) c |= 0xFF000000u;
            } else if (bpp == 2u) {
                uint16_t v = static_cast<uint16_t>(sp[0] | (sp[1] << 8));
                if (fmt == 0x05u || fmt == 0x11u) {
                    uint32_t r = ((v >> 11) & 31u) * 255u / 31u;
                    uint32_t g = ((v >> 5) & 63u) * 255u / 63u;
                    uint32_t b = (v & 31u) * 255u / 31u;
                    c = 0xFF000000u | (r << 16) | (g << 8) | b;
                } else if (fmt == 0x04u || fmt == 0x1Du) {
                    uint32_t a = ((v >> 12) & 15u) * 17u, r = ((v >> 8) & 15u) * 17u,
                             g = ((v >> 4) & 15u) * 17u, b = (v & 15u) * 17u;
                    c = (a << 24) | (r << 16) | (g << 8) | b;
                } else if (fmt == 0x1Au) {
                    uint32_t a = sp[1], l = sp[0];
                    c = (a << 24) | (l << 16) | (l << 8) | l;
                } else {
                    uint32_t a = (fmt == 0x03u || fmt == 0x10u) ? 255u : ((v & 0x8000u) ? 255u : 0u);
                    uint32_t r = ((v >> 10) & 31u) * 255u / 31u;
                    uint32_t g = ((v >> 5) & 31u) * 255u / 31u;
                    uint32_t b = (v & 31u) * 255u / 31u;
                    c = (a << 24) | (r << 16) | (g << 8) | b;
                }
            } else {
                uint8_t idx = sp[0];
                if (fmt == 0x0Bu && pal) c = pal[idx];
                else if (fmt == 0x01u) c = (uint32_t(idx) << 24) | 0x00FFFFFFu;
                else c = 0xFF000000u | (uint32_t(idx) << 16) | (uint32_t(idx) << 8) | idx;
            }
            out_bgra[static_cast<size_t>(y) * mw + x] = c;
        }
    }
    out_consumed = is_lin ? (static_cast<size_t>(mh) * pitch) : (static_cast<size_t>(mw) * mh * bpp);
}

// Software DXT1/3/5 fallback decoder (BGRA8 output) when GL_EXT_texture_compression_s3tc is unavailable
inline void xb_decode_dxt_to_bgra(const uint8_t* src, uint32_t w, uint32_t h, uint32_t fmt, std::vector<uint32_t>& out) {
    out.assign(static_cast<size_t>(w) * h, 0xFF000000u);
    uint32_t bw = std::max(1u, (w + 3u) / 4u);
    uint32_t bh = std::max(1u, (h + 3u) / 4u);
    uint32_t bpb = (fmt == 0x0Cu) ? 8u : 16u;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = src + (static_cast<size_t>(by) * bw + bx) * bpb;
            const uint8_t* cblk = (fmt == 0x0Cu) ? blk : (blk + 8);
            uint16_t c0 = static_cast<uint16_t>(cblk[0] | (cblk[1] << 8));
            uint16_t c1 = static_cast<uint16_t>(cblk[2] | (cblk[3] << 8));
            uint32_t lut = static_cast<uint32_t>(cblk[4] | (cblk[5] << 8) | (cblk[6] << 16) | (cblk[7] << 24));
            uint8_t r[4], g[4], b[4], a[4]{255, 255, 255, 255};
            r[0] = ((c0 >> 11) & 31) * 255 / 31; g[0] = ((c0 >> 5) & 63) * 255 / 63; b[0] = (c0 & 31) * 255 / 31;
            r[1] = ((c1 >> 11) & 31) * 255 / 31; g[1] = ((c1 >> 5) & 63) * 255 / 63; b[1] = (c1 & 31) * 255 / 31;
            if (fmt == 0x0Cu && c0 <= c1) {
                r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; b[2] = (b[0] + b[1]) / 2;
                r[3] = g[3] = b[3] = a[3] = 0;
            } else {
                r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; b[2] = (2 * b[0] + b[1]) / 3;
                r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; b[3] = (b[0] + 2 * b[1]) / 3;
            }
            uint64_t a5 = 0;
            uint8_t a0 = blk[0], a1 = blk[1];
            if (fmt == 0x0Fu) {
                std::memcpy(&a5, blk, 8);
                a5 >>= 16;
            }
            for (uint32_t py = 0; py < 4; ++py) {
                uint32_t yy = by * 4 + py;
                if (yy >= h) continue;
                for (uint32_t px = 0; px < 4; ++px) {
                    uint32_t xx = bx * 4 + px;
                    if (xx >= w) continue;
                    uint32_t pi = py * 4 + px;
                    uint32_t ci = (lut >> (pi * 2)) & 3u;
                    uint8_t alpha = a[ci];
                    if (fmt == 0x0Eu) {
                        uint8_t nib = (blk[pi >> 1] >> ((pi & 1) * 4)) & 0xFu;
                        alpha = nib * 17u;
                    } else if (fmt == 0x0Fu) {
                        uint32_t ai = (a5 >> (pi * 3)) & 7u;
                        if (ai == 0) alpha = a0;
                        else if (ai == 1) alpha = a1;
                        else if (a0 > a1) alpha = static_cast<uint8_t>(((8 - ai) * a0 + (ai - 1) * a1) / 7);
                        else if (ai == 6) alpha = 0;
                        else if (ai == 7) alpha = 255;
                        else alpha = static_cast<uint8_t>(((6 - ai) * a0 + (ai - 1) * a1) / 5);
                    }
                    out[static_cast<size_t>(yy) * w + xx] =
                        (uint32_t(alpha) << 24) | (uint32_t(r[ci]) << 16) | (uint32_t(g[ci]) << 8) | b[ci];
                }
            }
        }
    }
}
