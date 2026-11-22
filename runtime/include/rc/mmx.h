/* SPDX-License-Identifier: MIT */
/*
 * MMX operations for lifted code: pure functions on 64-bit values, lane by lane
 * (rc-ir's MmxOp). `a` is the destination's value, `b` the source's or a shift count.
 */
#ifndef RC_MMX_H
#define RC_MMX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lanes: get lane i of `bits` bits, put a value there. */
#define RC_LANE(v, bits, i) ((uint64_t)((v) >> ((bits) * (i))) & ((UINT64_C(1) << (bits)) - 1))
#define RC_PUT(v, bits, i, x) ((v) | (((uint64_t)(x) & ((UINT64_C(1) << (bits)) - 1)) << ((bits) * (i))))

static inline int64_t rc_mmx_sx(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }

static inline int64_t rc_mmx_sat(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* A lane-wise operation over all lanes of `bits` bits. */
#define RC_MMX_LANES(name, bits, expr)                                                             \
    static inline uint64_t rc_mmx_##name(uint64_t a, uint64_t b)                                   \
    {                                                                                              \
        uint64_t r = 0;                                                                            \
        for (int i = 0; i < 64 / (bits); i++) {                                                    \
            uint64_t x = RC_LANE(a, bits, i), y = RC_LANE(b, bits, i);                             \
            int64_t sx = rc_mmx_sx(x, bits), sy = rc_mmx_sx(y, bits);                              \
            (void)sx;                                                                              \
            (void)sy;                                                                              \
            r = RC_PUT(r, bits, i, (expr));                                                        \
        }                                                                                          \
        return r;                                                                                  \
    }

RC_MMX_LANES(paddb, 8, x + y)
RC_MMX_LANES(paddw, 16, x + y)
RC_MMX_LANES(paddd, 32, x + y)
RC_MMX_LANES(paddsb, 8, rc_mmx_sat(sx + sy, -128, 127))
RC_MMX_LANES(paddsw, 16, rc_mmx_sat(sx + sy, -32768, 32767))
RC_MMX_LANES(paddusb, 8, rc_mmx_sat((int64_t)(x + y), 0, 255))
RC_MMX_LANES(paddusw, 16, rc_mmx_sat((int64_t)(x + y), 0, 65535))
RC_MMX_LANES(psubb, 8, x - y)
RC_MMX_LANES(psubw, 16, x - y)
RC_MMX_LANES(psubd, 32, x - y)
RC_MMX_LANES(psubsb, 8, rc_mmx_sat(sx - sy, -128, 127))
RC_MMX_LANES(psubsw, 16, rc_mmx_sat(sx - sy, -32768, 32767))
RC_MMX_LANES(psubusb, 8, rc_mmx_sat((int64_t)x - (int64_t)y, 0, 255))
RC_MMX_LANES(psubusw, 16, rc_mmx_sat((int64_t)x - (int64_t)y, 0, 65535))
RC_MMX_LANES(pmullw, 16, (uint64_t)(sx * sy))
RC_MMX_LANES(pmulhw, 16, (uint64_t)((sx * sy) >> 16))
RC_MMX_LANES(pcmpeqb, 8, x == y ? 0xff : 0)
RC_MMX_LANES(pcmpeqw, 16, x == y ? 0xffff : 0)
RC_MMX_LANES(pcmpeqd, 32, x == y ? 0xffffffffu : 0)
RC_MMX_LANES(pcmpgtb, 8, sx > sy ? 0xff : 0)
RC_MMX_LANES(pcmpgtw, 16, sx > sy ? 0xffff : 0)
RC_MMX_LANES(pcmpgtd, 32, sx > sy ? 0xffffffffu : 0)

/* Pairs of signed 16-bit products, summed into 32-bit lanes. */
static inline uint64_t rc_mmx_pmaddwd(uint64_t a, uint64_t b)
{
    uint64_t r = 0;
    for (int i = 0; i < 2; i++) {
        int64_t s = rc_mmx_sx(RC_LANE(a, 16, 2 * i), 16) * rc_mmx_sx(RC_LANE(b, 16, 2 * i), 16) +
                    rc_mmx_sx(RC_LANE(a, 16, 2 * i + 1), 16) * rc_mmx_sx(RC_LANE(b, 16, 2 * i + 1), 16);
        r = RC_PUT(r, 32, i, (uint64_t)s);
    }
    return r;
}

/* Packs: the lanes of a, then those of b, narrowed with saturation. */
#define RC_MMX_PACK(name, from, to, lo, hi)                                                        \
    static inline uint64_t rc_mmx_##name(uint64_t a, uint64_t b)                                   \
    {                                                                                              \
        uint64_t r = 0;                                                                            \
        int n = 64 / (from);                                                                       \
        for (int i = 0; i < n; i++) {                                                              \
            r = RC_PUT(r, to, i, (uint64_t)rc_mmx_sat(rc_mmx_sx(RC_LANE(a, from, i), from), lo, hi)); \
            r = RC_PUT(r, to, n + i, (uint64_t)rc_mmx_sat(rc_mmx_sx(RC_LANE(b, from, i), from), lo, hi)); \
        }                                                                                          \
        return r;                                                                                  \
    }
RC_MMX_PACK(packsswb, 16, 8, -128, 127)
RC_MMX_PACK(packssdw, 32, 16, -32768, 32767)
RC_MMX_PACK(packuswb, 16, 8, 0, 255)

/* Unpacks: lanes of the low (or high) halves of a and b, interleaved. */
#define RC_MMX_UNPACK(name, bits, high)                                                            \
    static inline uint64_t rc_mmx_##name(uint64_t a, uint64_t b)                                   \
    {                                                                                              \
        uint64_t r = 0;                                                                            \
        int n = 32 / (bits);                                                                       \
        for (int i = 0; i < n; i++) {                                                              \
            r = RC_PUT(r, bits, 2 * i, RC_LANE(a, bits, (high) * n + i));                          \
            r = RC_PUT(r, bits, 2 * i + 1, RC_LANE(b, bits, (high) * n + i));                      \
        }                                                                                          \
        return r;                                                                                  \
    }
RC_MMX_UNPACK(punpcklbw, 8, 0)
RC_MMX_UNPACK(punpcklwd, 16, 0)
RC_MMX_UNPACK(punpckldq, 32, 0)
RC_MMX_UNPACK(punpckhbw, 8, 1)
RC_MMX_UNPACK(punpckhwd, 16, 1)
RC_MMX_UNPACK(punpckhdq, 32, 1)

/* Shifts by the whole count: at or past the lane width, zero (or the sign, arithmetic). */
#define RC_MMX_SHIFT(name, bits, op)                                                               \
    static inline uint64_t rc_mmx_##name(uint64_t a, uint64_t n)                                   \
    {                                                                                              \
        uint64_t r = 0;                                                                            \
        for (int i = 0; i < 64 / (bits); i++) {                                                    \
            uint64_t x = RC_LANE(a, bits, i);                                                      \
            r = RC_PUT(r, bits, i, (op));                                                          \
        }                                                                                          \
        return r;                                                                                  \
    }
RC_MMX_SHIFT(psllw, 16, n >= 16 ? 0 : x << n)
RC_MMX_SHIFT(pslld, 32, n >= 32 ? 0 : x << n)
RC_MMX_SHIFT(psrlw, 16, n >= 16 ? 0 : x >> n)
RC_MMX_SHIFT(psrld, 32, n >= 32 ? 0 : x >> n)
RC_MMX_SHIFT(psraw, 16, (uint64_t)(rc_mmx_sx(x, 16) >> (n >= 16 ? 15 : n)))
RC_MMX_SHIFT(psrad, 32, (uint64_t)(rc_mmx_sx(x, 32) >> (n >= 32 ? 31 : n)))
static inline uint64_t rc_mmx_psllq(uint64_t a, uint64_t n) { return n >= 64 ? 0 : a << n; }
static inline uint64_t rc_mmx_psrlq(uint64_t a, uint64_t n) { return n >= 64 ? 0 : a >> n; }

#ifdef __cplusplus
}
#endif
/* Every operation, in the order of rc_ir::MmxOp (recomp/crates/rc-ir): native code calls them by
 * that index (runtime/src/native.c). Keep the two in the same order. */
#define RC_MMX_OPS(X)                                                                                \
    X(paddb) X(paddw) X(paddd) X(paddsb) X(paddsw) X(paddusb) X(paddusw) X(psubb) X(psubw) X(psubd)  \
    X(psubsb) X(psubsw) X(psubusb) X(psubusw) X(pmullw) X(pmulhw) X(pmaddwd) X(pcmpeqb) X(pcmpeqw)   \
    X(pcmpeqd) X(pcmpgtb) X(pcmpgtw) X(pcmpgtd) X(packsswb) X(packssdw) X(packuswb) X(punpcklbw)     \
    X(punpcklwd) X(punpckldq) X(punpckhbw) X(punpckhwd) X(punpckhdq) X(psllw) X(pslld) X(psllq)     \
    X(psrlw) X(psrld) X(psrlq) X(psraw) X(psrad)

#endif
