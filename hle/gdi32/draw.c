/* SPDX-License-Identifier: MIT */
/*
 * gdi32: drawing. Every operation goes through here: colors to the target's pixels and back,
 * clipping to the DC's region (the window's visible part, the program's clip region), fills,
 * patterns, blits with any of the 256 raster operations, DIBs, lines and shapes, and the DC's
 * state (SaveDC, origins, modes).
 *
 * Coordinates: logical (what the program passes), device (after the viewport and window
 * origins, rc_dc_x/y), target (device + the window's place in the target, org_x/y).
 */
#include <stdlib.h>
#include <string.h>

#include "dc.h"
#include "gdi32.h"
#include "objects.h"
#include "rc/hle.h"
#include "rc/log.h"
#include "rc/vm.h"

#define G32 "GDI32.dll"

/* ---- pixels ---- */

static uint32_t channel(uint32_t v, uint32_t mask)
{
    if (!mask) {
        return 0;
    }
    int shift = 0, bits = 0;
    while (!((mask >> shift) & 1)) {
        shift++;
    }
    while (shift + bits < 32 && ((mask >> (shift + bits)) & 1)) {
        bits++;
    }
    return ((v * ((1u << bits) - 1) + 127) / 255) << shift;
}

static uint32_t unchannel(uint32_t p, uint32_t mask)
{
    if (!mask) {
        return 0;
    }
    int shift = 0;
    while (!((mask >> shift) & 1)) {
        shift++;
    }
    uint32_t max = mask >> shift;
    return ((p & mask) >> shift) * 255 / max;
}

static void masks_of(const rc_pixels *t, uint32_t *rm, uint32_t *gm, uint32_t *bm)
{
    *rm = t->rmask, *gm = t->gmask, *bm = t->bmask;
    if (!*rm && !*gm && !*bm) {
        if (t->bpp == 16) {
            *rm = 0xf800, *gm = 0x07e0, *bm = 0x001f;
        } else {
            *rm = 0xff0000, *gm = 0xff00, *bm = 0xff;
        }
    }
}

uint32_t rc_dc_pixel(const rc_dc *d, uint32_t ref)
{
    uint32_t r = ref & 0xff, g = (ref >> 8) & 0xff, b = (ref >> 16) & 0xff;
    switch (d->target.bpp) {
    case 1:
        return (r * 30 + g * 59 + b * 11) / 100 >= 128 ? 1u : 0u;
    case 4:
    case 8: {
        if (!d->has_palette) {
            return (r * 30 + g * 59 + b * 11) / 100;
        }
        static const rc_dc *cache_dc;
        static uint32_t cache_ref = 0xffffffffu, cache_px;
        if (cache_dc == d && cache_ref == ref) {
            return cache_px;
        }
        uint32_t best = 0, dist = 0xffffffffu, n = d->target.bpp == 4 ? 16 : 256;
        for (uint32_t i = 0; i < n && dist; i++) {
            uint32_t q = d->palette[i];
            int dr = (int)((q >> 16) & 0xff) - (int)r, dg = (int)((q >> 8) & 0xff) - (int)g,
                db = (int)(q & 0xff) - (int)b;
            uint32_t e = (uint32_t)(dr * dr + dg * dg + db * db);
            if (e < dist) {
                dist = e;
                best = i;
            }
        }
        cache_dc = d;
        cache_ref = ref;
        cache_px = best;
        return best;
    }
    default: {
        uint32_t rm, gm, bm;
        masks_of(&d->target, &rm, &gm, &bm);
        return channel(r, rm) | channel(g, gm) | channel(b, bm);
    }
    }
}

uint32_t rc_dc_colorref(const rc_dc *d, uint32_t p)
{
    switch (d->target.bpp) {
    case 1:
        return p ? 0xffffffu : 0;
    case 4:
    case 8: {
        if (!d->has_palette) {
            return (p & 0xff) * 0x010101u;
        }
        uint32_t q = d->palette[p & 0xff];
        return ((q >> 16) & 0xff) | (q & 0xff00) | (q & 0xff) << 16;
    }
    default: {
        uint32_t rm, gm, bm;
        masks_of(&d->target, &rm, &gm, &bm);
        return unchannel(p, rm) | unchannel(p, gm) << 8 | unchannel(p, bm) << 16;
    }
    }
}

uint32_t rc_dc_read(const rc_dc *d, int32_t x, int32_t y)
{
    const rc_pixels *t = &d->target;
    if (!t->pixels || x < 0 || y < 0 || (uint32_t)x >= t->w || (uint32_t)y >= t->h) {
        return 0;
    }
    uint32_t row = t->pixels + (uint32_t)y * t->pitch;
    switch (t->bpp) {
    case 1: return (rc_ld8(row + (uint32_t)x / 8) >> (7 - (x & 7))) & 1;
    case 4: return (rc_ld8(row + (uint32_t)x / 2) >> ((x & 1) ? 0 : 4)) & 15;
    case 8: return rc_ld8(row + (uint32_t)x);
    case 16: return rc_ld16(row + 2 * (uint32_t)x);
    case 24: return rc_ld16(row + 3 * (uint32_t)x) | (uint32_t)rc_ld8(row + 3 * (uint32_t)x + 2) << 16;
    default: return rc_ld32(row + 4 * (uint32_t)x);
    }
}

void rc_dc_write(const rc_dc *d, int32_t x, int32_t y, uint32_t v)
{
    const rc_pixels *t = &d->target;
    if (!t->pixels || x < 0 || y < 0 || (uint32_t)x >= t->w || (uint32_t)y >= t->h) {
        return;
    }
    uint32_t row = t->pixels + (uint32_t)y * t->pitch;
    switch (t->bpp) {
    case 1: {
        uint32_t a = row + (uint32_t)x / 8;
        uint8_t m = (uint8_t)(0x80u >> (x & 7));
        rc_st8(a, (uint8_t)(v ? rc_ld8(a) | m : rc_ld8(a) & ~m));
        break;
    }
    case 4: {
        uint32_t a = row + (uint32_t)x / 2;
        uint8_t o = rc_ld8(a);
        rc_st8(a, (uint8_t)((x & 1) ? (o & 0xf0) | (v & 15) : (o & 0x0f) | (v & 15) << 4));
        break;
    }
    case 8: rc_st8(row + (uint32_t)x, (uint8_t)v); break;
    case 16: rc_st16(row + 2 * (uint32_t)x, (uint16_t)v); break;
    case 24:
        rc_st16(row + 3 * (uint32_t)x, (uint16_t)v);
        rc_st8(row + 3 * (uint32_t)x + 2, (uint8_t)(v >> 16));
        break;
    default: rc_st32(row + 4 * (uint32_t)x, v); break;
    }
}

/* ---- where a DC draws ---- */

void rc_dc_region(const rc_dc *d, rc_rgn *out)
{
    if (d->vis.n >= 0) {
        *out = d->vis;
    } else {
        rc_rgn_set(out, 0, 0, (int32_t)d->target.w, (int32_t)d->target.h);
    }
    if (d->has_clip) {
        rc_rgn c = d->clip;
        rc_rgn_offset(&c, d->org_x, d->org_y);
        rc_rgn_and(out, &c);
    }
    rc_rgn_and_rect(out, (rc_rect){0, 0, (int32_t)d->target.w, (int32_t)d->target.h});
}

void rc_dc_fill(rc_dc *d, rc_rect r, uint32_t colorref)
{
    if (!d->target.pixels) {
        return;
    }
    rc_rgn g;
    rc_dc_region(d, &g);
    rc_rgn_and_rect(&g, (rc_rect){r.l + d->org_x, r.t + d->org_y, r.r + d->org_x, r.b + d->org_y});
    uint32_t v = rc_dc_pixel(d, colorref);
    for (int i = 0; i < g.n; i++) {
        rc_rect x = g.r[i];
        if (d->target.bpp == 32) {
            for (int32_t y = x.t; y < x.b; y++) {
                uint32_t row = d->target.pixels + (uint32_t)y * d->target.pitch + 4 * (uint32_t)x.l;
                for (int32_t k = 0; k < x.r - x.l; k++) {
                    rc_st32(row + 4 * (uint32_t)k, v);
                }
            }
            continue;
        }
        for (int32_t y = x.t; y < x.b; y++) {
            for (int32_t k = x.l; k < x.r; k++) {
                rc_dc_write(d, k, y, v);
            }
        }
    }
}

void rc_dc_plot(rc_dc *d, int32_t x, int32_t y, uint32_t pixel)
{
    int32_t tx = x + d->org_x, ty = y + d->org_y;
    if (d->vis.n >= 0 && !rc_rgn_has(&d->vis, tx, ty)) {
        return;
    }
    if (d->has_clip && !rc_rgn_has(&d->clip, x, y)) {
        return;
    }
    rc_dc_write(d, tx, ty, pixel);
}

void rc_gdi_fill(uint32_t hdc, int32_t l, int32_t t, int32_t r, int32_t b, uint32_t colorref)
{
    rc_dc *d = rc_dc_get(hdc);
    if (d) {
        rc_dc_fill(d, (rc_rect){rc_dc_x(d, l), rc_dc_y(d, t), rc_dc_x(d, r), rc_dc_y(d, b)}, colorref);
    }
}

/* ---- raster operations ---- */

/* The result of ternary operation `rop` (its index, bits 16-23 of the code) on pattern, source
 * and destination, bit by bit. */
static uint32_t rop3(uint32_t rop, uint32_t p, uint32_t s, uint32_t dst)
{
    uint32_t r = 0, idx = (rop >> 16) & 0xff;
    for (uint32_t k = 0; k < 8; k++) {
        if (idx & (1u << k)) {
            r |= ((k & 4) ? p : ~p) & ((k & 2) ? s : ~s) & ((k & 1) ? dst : ~dst);
        }
    }
    return r & 0xffffff;
}

static bool rop_uses(uint32_t rop, int which) /* 0 D, 1 S, 2 P */
{
    uint32_t idx = (rop >> 16) & 0xff;
    for (uint32_t k = 0; k < 8; k++) {
        if (((idx >> k) & 1) != ((idx >> (k ^ (1u << which))) & 1)) {
            return true;
        }
    }
    return false;
}

/* The brush's color at target pixel (x, y). */
typedef struct brush_src {
    int kind; /* 0 none, 1 solid, 2 pattern */
    uint32_t color;
    rc_dc pat; /* a DC on the pattern bitmap */
    uint32_t text, bk;
} brush_src;

static void brush_of(rc_dc *d, uint32_t h, brush_src *b)
{
    gobj *o = rc_gobj_get(h ? h : (d->brush ? d->brush : rc_gdi_stock(0)), OB_BRUSH);
    memset(b, 0, sizeof *b);
    if (!o || o->style == 1) {
        return;
    }
    gobj *pb = o->style == 3 ? rc_gobj_get(o->pattern, OB_BITMAP) : NULL;
    if (pb && pb->pixels) {
        b->kind = 2;
        rc_gdi_bitmap_pixels(pb, &b->pat.target);
        b->pat.vis.n = -1;
        if (pb->ncolors) {
            for (uint32_t i = 0; i < pb->ncolors; i++) {
                b->pat.palette[i] = pb->colors[i];
            }
            b->pat.has_palette = 1;
        }
        b->text = d->text_color;
        b->bk = d->bk_color;
        return;
    }
    b->kind = 1;
    b->color = o->color;
}

static uint32_t brush_at(const brush_src *b, int32_t x, int32_t y)
{
    if (b->kind != 2) {
        return b->color;
    }
    const rc_pixels *t = &b->pat.target;
    int32_t px = (int32_t)((uint32_t)x % t->w), py = (int32_t)((uint32_t)y % t->h);
    uint32_t v = rc_dc_read(&b->pat, px, py);
    if (t->bpp == 1) {
        return v ? b->bk : b->text; /* a monochrome pattern: 1 background, 0 text color */
    }
    return rc_dc_colorref(&b->pat, v);
}

void rc_dc_pat(rc_dc *d, rc_rect r, uint32_t brush, uint32_t rop)
{
    if (!d->target.pixels) {
        return;
    }
    brush_src b;
    brush_of(d, brush, &b);
    if (!b.kind && rop_uses(rop, 2)) {
        return; /* a null brush paints nothing */
    }
    if (b.kind == 1 && rop == 0x00f00021u) { /* PATCOPY with a solid brush */
        rc_dc_fill(d, r, b.color);
        return;
    }
    rc_rgn g;
    rc_dc_region(d, &g);
    rc_rgn_and_rect(&g, (rc_rect){r.l + d->org_x, r.t + d->org_y, r.r + d->org_x, r.b + d->org_y});
    bool use_d = rop_uses(rop, 0);
    for (int i = 0; i < g.n; i++) {
        for (int32_t y = g.r[i].t; y < g.r[i].b; y++) {
            for (int32_t x = g.r[i].l; x < g.r[i].r; x++) {
                uint32_t dst = use_d ? rc_dc_colorref(d, rc_dc_read(d, x, y)) : 0;
                rc_dc_write(d, x, y, rc_dc_pixel(d, rop3(rop, brush_at(&b, x, y), 0, dst)));
            }
        }
    }
}

/* Copy (and scale) from `s` to `d` with raster operation `rop`, device coordinates of each. */
static void blit(rc_dc *d, int32_t dx, int32_t dy, int32_t dw, int32_t dh, rc_dc *s, int32_t sx, int32_t sy,
                 int32_t sw, int32_t sh, uint32_t rop)
{
    if (!d->target.pixels || !dw || !dh) {
        return;
    }
    bool use_s = rop_uses(rop, 1), use_d = rop_uses(rop, 0), use_p = rop_uses(rop, 2);
    if (use_s && (!s || !s->target.pixels)) {
        return;
    }
    /* Mirrored copies: negative sizes. */
    bool flip_x = (dw < 0) != (sw < 0), flip_y = (dh < 0) != (sh < 0);
    if (dw < 0) {
        dx += dw;
        dw = -dw;
    }
    if (dh < 0) {
        dy += dh;
        dh = -dh;
    }
    if (sw < 0) {
        sx += sw;
        sw = -sw;
    }
    if (sh < 0) {
        sy += sh;
        sh = -sh;
    }
    brush_src b;
    if (use_p) {
        brush_of(d, 0, &b);
    }
    rc_rgn g;
    rc_dc_region(d, &g);
    rc_rgn_and_rect(&g, (rc_rect){dx + d->org_x, dy + d->org_y, dx + dw + d->org_x, dy + dh + d->org_y});
    bool raw = use_s && rop == 0x00cc0020u && s->target.bpp == d->target.bpp && s->target.rmask == d->target.rmask &&
               s->target.gmask == d->target.gmask && s->target.bmask == d->target.bmask &&
               (d->target.bpp > 8 || !memcmp(s->palette, d->palette, sizeof d->palette));
    bool src_mono = use_s && s->target.bpp == 1 && d->target.bpp != 1;
    bool dst_mono = use_s && d->target.bpp == 1 && s->target.bpp != 1;
    for (int i = 0; i < g.n; i++) {
        for (int32_t ty = g.r[i].t; ty < g.r[i].b; ty++) {
            int32_t oy = ty - d->org_y - dy;
            int32_t ry = flip_y ? dh - 1 - oy : oy;
            int32_t py = sy + (sh == dh ? ry : (int32_t)((int64_t)ry * sh / dh));
            for (int32_t tx = g.r[i].l; tx < g.r[i].r; tx++) {
                int32_t ox = tx - d->org_x - dx;
                int32_t rx = flip_x ? dw - 1 - ox : ox;
                int32_t px = sx + (sw == dw ? rx : (int32_t)((int64_t)rx * sw / dw));
                int32_t stx = use_s ? px + s->org_x : 0, sty = use_s ? py + s->org_y : 0;
                if (raw) {
                    if (stx >= 0 && sty >= 0 && (uint32_t)stx < s->target.w && (uint32_t)sty < s->target.h) {
                        rc_dc_write(d, tx, ty, rc_dc_read(s, stx, sty));
                    }
                    continue;
                }
                uint32_t sv = 0;
                if (use_s) {
                    uint32_t raw_s = rc_dc_read(s, stx, sty);
                    if (src_mono) {
                        sv = raw_s ? d->bk_color : d->text_color; /* 1 background, 0 text color */
                    } else {
                        sv = rc_dc_colorref(s, raw_s);
                        if (dst_mono) {
                            sv = sv == (s->bk_color & 0xffffff) ? 0xffffff : 0; /* background becomes 1 */
                        }
                    }
                }
                uint32_t dv = use_d ? rc_dc_colorref(d, rc_dc_read(d, tx, ty)) : 0;
                uint32_t pv = use_p ? brush_at(&b, tx, ty) : 0;
                rc_dc_write(d, tx, ty, rc_dc_pixel(d, rop3(rop, pv, sv, dv)));
            }
        }
    }
}

/* BitBlt(dst, x, y, w, h, src, sx, sy, rop) */
static void k_BitBlt(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0)), *s = rc_dc_get(RC_ARG(c, 5));
    if (!d) {
        RC_RETURN(c, 0, 9);
    }
    int32_t x = rc_dc_x(d, (int32_t)RC_ARG(c, 1)), y = rc_dc_y(d, (int32_t)RC_ARG(c, 2));
    int32_t w = (int32_t)RC_ARG(c, 3), h = (int32_t)RC_ARG(c, 4);
    int32_t sx = s ? rc_dc_x(s, (int32_t)RC_ARG(c, 6)) : 0, sy = s ? rc_dc_y(s, (int32_t)RC_ARG(c, 7)) : 0;
    blit(d, x, y, w, h, s, sx, sy, w, h, RC_ARG(c, 8));
    RC_RETURN(c, 1, 9);
}

/* StretchBlt(dst, x, y, w, h, src, sx, sy, sw, sh, rop) */
static void k_StretchBlt(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0)), *s = rc_dc_get(RC_ARG(c, 5));
    if (!d) {
        RC_RETURN(c, 0, 11);
    }
    blit(d, rc_dc_x(d, (int32_t)RC_ARG(c, 1)), rc_dc_y(d, (int32_t)RC_ARG(c, 2)), (int32_t)RC_ARG(c, 3),
         (int32_t)RC_ARG(c, 4), s, s ? rc_dc_x(s, (int32_t)RC_ARG(c, 6)) : 0, s ? rc_dc_y(s, (int32_t)RC_ARG(c, 7)) : 0,
         (int32_t)RC_ARG(c, 8), (int32_t)RC_ARG(c, 9), RC_ARG(c, 10));
    RC_RETURN(c, 1, 11);
}

/* PatBlt(dc, x, y, w, h, rop) */
static void k_PatBlt(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, 0, 6);
    }
    int32_t x = rc_dc_x(d, (int32_t)RC_ARG(c, 1)), y = rc_dc_y(d, (int32_t)RC_ARG(c, 2));
    int32_t w = (int32_t)RC_ARG(c, 3), h = (int32_t)RC_ARG(c, 4);
    if (w < 0) {
        x += w;
        w = -w;
    }
    if (h < 0) {
        y += h;
        h = -h;
    }
    rc_dc_pat(d, (rc_rect){x, y, x + w, y + h}, 0, RC_ARG(c, 5));
    RC_RETURN(c, 1, 6);
}

/* ---- DIBs ---- */

/* A DIB to draw from: a DC on its pixels (with its color table as the palette). */
static bool dib_dc(uint32_t bits, uint32_t bmi, uint32_t usage, const rc_dc *pal_from, rc_dc *out)
{
    int32_t w = (int32_t)rc_ld32(bmi + 4), h = (int32_t)rc_ld32(bmi + 8);
    uint32_t hsize = rc_ld32(bmi), bpp = rc_ld16(bmi + 14), comp = rc_ld32(bmi + 16), used = rc_ld32(bmi + 32);
    if (w <= 0 || h == 0 || !bits || (comp != 0 && comp != 3)) {
        return false;
    }
    memset(out, 0, sizeof *out);
    uint32_t ah = (uint32_t)(h < 0 ? -h : h), wb = (((uint32_t)w * bpp + 31) / 32) * 4;
    uint32_t base = bits, pitch = wb;
    if (h > 0) {
        base = bits + (ah - 1) * wb;
        pitch = (uint32_t)-(int32_t)wb;
    }
    uint32_t rm = 0, gm = 0, bm = 0;
    if (bpp == 16) {
        rm = 0x7c00, gm = 0x03e0, bm = 0x001f;
    } else if (bpp >= 24) {
        rm = 0xff0000, gm = 0xff00, bm = 0xff;
    }
    if (comp == 3) {
        rm = rc_ld32(bmi + 40), gm = rc_ld32(bmi + 44), bm = rc_ld32(bmi + 48);
    }
    out->target = (rc_pixels){base, (uint32_t)w, ah, pitch, bpp, rm, gm, bm};
    out->vis.n = -1;
    if (bpp <= 8) {
        uint32_t n = used ? used : 1u << bpp;
        for (uint32_t i = 0; i < n && i < 256; i++) {
            if (usage == 1 && pal_from) {
                out->palette[i] = pal_from->palette[rc_ld16(bmi + hsize + 2 * i) & 0xff];
            } else {
                out->palette[i] = rc_ld32(bmi + hsize + 4 * i) & 0xffffff;
            }
        }
        out->has_palette = 1;
        if (bpp == 1) {
            out->target.bpp = 8; /* read 1-bit DIBs through their 2-color table */
            out->target.w = (uint32_t)w;
        }
    }
    return true;
}

/* Read pixel (x, y) of a DIB DC as a COLORREF (1-bit DIBs read bit by bit). */
static uint32_t dib_ref(const rc_dc *s, uint32_t bmi, int32_t x, int32_t y)
{
    if (rc_ld16(bmi + 14) == 1) {
        uint32_t row = s->target.pixels + (uint32_t)y * s->target.pitch;
        uint32_t bit = (rc_ld8(row + (uint32_t)x / 8) >> (7 - (x & 7))) & 1;
        uint32_t q = s->palette[bit];
        return ((q >> 16) & 0xff) | (q & 0xff00) | (q & 0xff) << 16;
    }
    return rc_dc_colorref(s, rc_dc_read(s, x, y));
}

static void draw_dib(rc_dc *d, int32_t dx, int32_t dy, int32_t dw, int32_t dh, uint32_t bits, uint32_t bmi,
                     uint32_t usage, int32_t sx, int32_t sy, int32_t sw, int32_t sh, uint32_t rop)
{
    rc_dc s;
    if (!d->target.pixels || !dib_dc(bits, bmi, usage, d, &s) || !dw || !dh || !sw || !sh) {
        return;
    }
    bool flip_x = (dw < 0) != (sw < 0), flip_y = (dh < 0) != (sh < 0);
    if (dw < 0) {
        dx += dw;
        dw = -dw;
    }
    if (dh < 0) {
        dy += dh;
        dh = -dh;
    }
    if (sw < 0) {
        sx += sw;
        sw = -sw;
    }
    if (sh < 0) {
        sy += sh;
        sh = -sh;
    }
    rc_rgn g;
    rc_dc_region(d, &g);
    rc_rgn_and_rect(&g, (rc_rect){dx + d->org_x, dy + d->org_y, dx + dw + d->org_x, dy + dh + d->org_y});
    bool use_d = rop_uses(rop, 0);
    bool raw = rop == 0x00cc0020u && rc_ld16(bmi + 14) == d->target.bpp && d->target.bpp >= 16 &&
               s.target.rmask == d->target.rmask && s.target.gmask == d->target.gmask && s.target.bmask == d->target.bmask;
    for (int i = 0; i < g.n; i++) {
        for (int32_t ty = g.r[i].t; ty < g.r[i].b; ty++) {
            int32_t oy = ty - d->org_y - dy, ry = flip_y ? dh - 1 - oy : oy;
            int32_t py = sy + (sh == dh ? ry : (int32_t)((int64_t)ry * sh / dh));
            if (py < 0 || (uint32_t)py >= s.target.h) {
                continue;
            }
            for (int32_t tx = g.r[i].l; tx < g.r[i].r; tx++) {
                int32_t ox = tx - d->org_x - dx, rx = flip_x ? dw - 1 - ox : ox;
                int32_t px = sx + (sw == dw ? rx : (int32_t)((int64_t)rx * sw / dw));
                if (px < 0 || (uint32_t)px >= s.target.w) {
                    continue;
                }
                if (raw) {
                    rc_dc_write(d, tx, ty, rc_dc_read(&s, px, py));
                    continue;
                }
                uint32_t sv = dib_ref(&s, bmi, px, py);
                uint32_t dv = use_d ? rc_dc_colorref(d, rc_dc_read(d, tx, ty)) : 0;
                rc_dc_write(d, tx, ty, rc_dc_pixel(d, rop3(rop, 0, sv, dv)));
            }
        }
    }
}

void rc_gdi_draw_dib_into(uint32_t hbitmap, uint32_t bits, uint32_t bmi, uint32_t usage)
{
    gobj *b = rc_gobj_get(hbitmap, OB_BITMAP);
    if (!b) {
        return;
    }
    rc_dc d;
    memset(&d, 0, sizeof d);
    rc_gdi_bitmap_pixels(b, &d.target);
    d.vis.n = -1;
    int32_t h = (int32_t)rc_ld32(bmi + 8);
    draw_dib(&d, 0, 0, (int32_t)b->w, (int32_t)b->h, bits, bmi, usage, 0, 0, (int32_t)rc_ld32(bmi + 4), h < 0 ? -h : h,
             0x00cc0020u);
}

/* StretchDIBits(dc, x, y, w, h, sx, sy, sw, sh, bits, bmi, usage, rop): the source rectangle is
 * counted from the DIB's top as it is shown. */
static void k_StretchDIBits(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, 0, 13);
    }
    int32_t h = (int32_t)rc_ld32(RC_ARG(c, 10) + 8), ah = h < 0 ? -h : h;
    int32_t sy = (int32_t)RC_ARG(c, 6), sh = (int32_t)RC_ARG(c, 8);
    if (h > 0) {
        sy = ah - sy - sh; /* bottom-up: y counts from the bottom row */
    }
    draw_dib(d, rc_dc_x(d, (int32_t)RC_ARG(c, 1)), rc_dc_y(d, (int32_t)RC_ARG(c, 2)), (int32_t)RC_ARG(c, 3),
             (int32_t)RC_ARG(c, 4), RC_ARG(c, 9), RC_ARG(c, 10), RC_ARG(c, 11), (int32_t)RC_ARG(c, 5), sy,
             (int32_t)RC_ARG(c, 7), sh, RC_ARG(c, 12));
    RC_RETURN(c, (uint32_t)ah, 13);
}

/* SetDIBitsToDevice(dc, x, y, w, h, sx, sy, start, lines, bits, bmi, usage) */
static void k_SetDIBitsToDevice(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, 0, 12);
    }
    int32_t h = (int32_t)rc_ld32(RC_ARG(c, 10) + 8), ah = h < 0 ? -h : h;
    int32_t w = (int32_t)RC_ARG(c, 3), hh = (int32_t)RC_ARG(c, 4), sy = (int32_t)RC_ARG(c, 6);
    if (h > 0) {
        sy = ah - sy - hh;
    }
    draw_dib(d, rc_dc_x(d, (int32_t)RC_ARG(c, 1)), rc_dc_y(d, (int32_t)RC_ARG(c, 2)), w, hh, RC_ARG(c, 9),
             RC_ARG(c, 10), RC_ARG(c, 11), (int32_t)RC_ARG(c, 5), sy, w, hh, 0x00cc0020u);
    RC_RETURN(c, RC_ARG(c, 8), 12);
}

/* ---- lines and shapes ---- */

static void pen_of(const rc_dc *d, uint32_t *style, uint32_t *width, uint32_t *color)
{
    gobj *p = rc_gobj_get(d->pen ? d->pen : rc_gdi_stock(7), OB_PEN);
    *style = p ? p->style : 0;
    *width = p ? p->pwidth : 1;
    *color = p ? p->color : 0;
}

/* A pixel of the pen with the DC's ROP2 (R2_BLACK 1 ... R2_WHITE 16). */
static void pen_plot(rc_dc *d, int32_t x, int32_t y, uint32_t color)
{
    uint32_t rop2 = d->rop2 ? d->rop2 : 13, v;
    if (rop2 == 13) {
        rc_dc_plot(d, x, y, rc_dc_pixel(d, color));
        return;
    }
    int32_t tx = x + d->org_x, ty = y + d->org_y;
    uint32_t dst = rc_dc_colorref(d, rc_dc_read(d, tx, ty)), p = color;
    switch (rop2) {
    case 1: v = 0; break;
    case 6: v = ~dst; break;
    case 7: v = p ^ dst; break;
    case 9: v = p & dst; break;
    case 10: v = ~(p ^ dst); break;
    case 11: return;
    case 15: v = p | dst; break;
    case 16: v = 0xffffff; break;
    default: v = p; break;
    }
    rc_dc_plot(d, x, y, rc_dc_pixel(d, v & 0xffffff));
}

/* A line in device coordinates without its last point, as GDI draws it. */
static void line(rc_dc *d, int32_t x, int32_t y, int32_t x1, int32_t y1)
{
    uint32_t style, width, color;
    pen_of(d, &style, &width, &color);
    if (style == 5 || !d->target.pixels) {
        return;
    }
    int32_t dx = x1 > x ? x1 - x : x - x1, dy = y1 > y ? y - y1 : y1 - y;
    int32_t sx = x < x1 ? 1 : -1, sy = y < y1 ? 1 : -1, err = dx + dy;
    int32_t lo = -(int32_t)(width - 1) / 2, hi = lo + (int32_t)width;
    while (x != x1 || y != y1) {
        for (int32_t j = lo; j < hi; j++) {
            for (int32_t i = lo; i < hi; i++) {
                pen_plot(d, x + i, y + j, color);
            }
        }
        int32_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y += sy;
        }
    }
}

static void k_MoveToEx(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, 0, 4);
    }
    uint32_t old = RC_ARG(c, 3);
    if (old) {
        rc_st32(old, (uint32_t)d->cur_x);
        rc_st32(old + 4, (uint32_t)d->cur_y);
    }
    d->cur_x = (int32_t)RC_ARG(c, 1);
    d->cur_y = (int32_t)RC_ARG(c, 2);
    RC_RETURN(c, 1, 4);
}

static void k_LineTo(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    int32_t x1 = (int32_t)RC_ARG(c, 1), y1 = (int32_t)RC_ARG(c, 2);
    if (!d) {
        RC_RETURN(c, 0, 3);
    }
    line(d, rc_dc_x(d, d->cur_x), rc_dc_y(d, d->cur_y), rc_dc_x(d, x1), rc_dc_y(d, y1));
    d->cur_x = x1;
    d->cur_y = y1;
    RC_RETURN(c, 1, 3);
}

static void k_GetCurrentPositionEx(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, 0, 2);
    }
    rc_st32(RC_ARG(c, 1), (uint32_t)d->cur_x);
    rc_st32(RC_ARG(c, 1) + 4, (uint32_t)d->cur_y);
    RC_RETURN(c, 1, 2);
}

/* Rectangle(dc, l, t, r, b): the brush inside, the pen's frame. */
static uint32_t rectangle(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        return 0;
    }
    int32_t l = rc_dc_x(d, (int32_t)RC_ARG(c, 1)), t = rc_dc_y(d, (int32_t)RC_ARG(c, 2));
    int32_t r = rc_dc_x(d, (int32_t)RC_ARG(c, 3)), b = rc_dc_y(d, (int32_t)RC_ARG(c, 4));
    uint32_t style, width, color;
    pen_of(d, &style, &width, &color);
    int32_t in = style == 5 ? 0 : 1;
    rc_dc_pat(d, (rc_rect){l + in, t + in, r - in, b - in}, 0, 0x00f00021u);
    if (style != 5) {
        line(d, l, t, r - 1, t);
        line(d, r - 1, t, r - 1, b - 1);
        line(d, r - 1, b - 1, l, b - 1);
        line(d, l, b - 1, l, t);
    }
    return 1;
}

static void k_Rectangle(rc_cpu *c) { RC_RETURN(c, rectangle(c), 5); }
/* RoundRect(dc, l, t, r, b, w, h): drawn as a rectangle. */
static void k_RoundRect(rc_cpu *c) { RC_RETURN(c, rectangle(c), 7); }

static int32_t isqrt(int64_t v)
{
    int64_t r = 0;
    while ((r + 1) * (r + 1) <= v) {
        r++;
    }
    return (int32_t)r;
}

/* Ellipse(dc, l, t, r, b) */
static void k_Ellipse(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, 0, 5);
    }
    int32_t l = rc_dc_x(d, (int32_t)RC_ARG(c, 1)), t = rc_dc_y(d, (int32_t)RC_ARG(c, 2));
    int32_t r = rc_dc_x(d, (int32_t)RC_ARG(c, 3)), b = rc_dc_y(d, (int32_t)RC_ARG(c, 4));
    /* In half pixels: centre (cx, cy), radii (ax, ay). */
    int64_t cx = l + r - 1, cy = t + b - 1, ax = r - l, ay = b - t;
    uint32_t style, width, color;
    pen_of(d, &style, &width, &color);
    for (int32_t y = t; y < b && ax > 0 && ay > 0; y++) {
        int64_t fy = 2 * (int64_t)y - cy, k = ay * ay - fy * fy;
        if (k < 0) {
            continue;
        }
        int32_t half = (int32_t)(ax * isqrt(k) / ay); /* half pixels */
        int32_t x0 = (int32_t)((cx - half + 1) / 2), x1 = (int32_t)((cx + half + 1) / 2);
        rc_dc_pat(d, (rc_rect){x0 + 1, y, x1, y + 1}, 0, 0x00f00021u);
        if (style != 5) {
            pen_plot(d, x0, y, color);
            pen_plot(d, x1, y, color);
        }
    }
    RC_RETURN(c, 1, 5);
}

/* Polyline(dc, points, n) */
static void k_Polyline(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t p = RC_ARG(c, 1), n = RC_ARG(c, 2);
    if (!d || n < 2) {
        RC_RETURN(c, d != NULL, 3);
    }
    for (uint32_t i = 0; i + 1 < n; i++) {
        line(d, rc_dc_x(d, (int32_t)rc_ld32(p + 8 * i)), rc_dc_y(d, (int32_t)rc_ld32(p + 8 * i + 4)),
             rc_dc_x(d, (int32_t)rc_ld32(p + 8 * i + 8)), rc_dc_y(d, (int32_t)rc_ld32(p + 8 * i + 12)));
    }
    RC_RETURN(c, 1, 3);
}

/* Polygon(dc, points, n): filled even-odd, then outlined. */
static void k_Polygon(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t p = RC_ARG(c, 1), n = RC_ARG(c, 2);
    if (!d || n < 2 || n > 4096) {
        RC_RETURN(c, d != NULL, 3);
    }
    int32_t *xs = malloc(sizeof(int32_t) * n), *ys = malloc(sizeof(int32_t) * n);
    int32_t top = 0x7fffffff, bottom = -0x7fffffff;
    for (uint32_t i = 0; xs && ys && i < n; i++) {
        xs[i] = rc_dc_x(d, (int32_t)rc_ld32(p + 8 * i));
        ys[i] = rc_dc_y(d, (int32_t)rc_ld32(p + 8 * i + 4));
        top = ys[i] < top ? ys[i] : top;
        bottom = ys[i] > bottom ? ys[i] : bottom;
    }
    for (int32_t y = top; xs && ys && y < bottom; y++) {
        int32_t cross[64], k = 0;
        for (uint32_t i = 0; i < n && k < 64; i++) {
            uint32_t j = (i + 1) % n;
            int32_t ya = ys[i], yb = ys[j];
            if ((y >= ya && y < yb) || (y >= yb && y < ya)) {
                cross[k++] = xs[i] + (int32_t)((int64_t)(y - ya) * (xs[j] - xs[i]) / (yb - ya));
            }
        }
        for (int32_t a = 1; a < k; a++) {
            for (int32_t b = a; b > 0 && cross[b - 1] > cross[b]; b--) {
                int32_t t = cross[b];
                cross[b] = cross[b - 1];
                cross[b - 1] = t;
            }
        }
        for (int32_t a = 0; a + 1 < k; a += 2) {
            rc_dc_pat(d, (rc_rect){cross[a], y, cross[a + 1], y + 1}, 0, 0x00f00021u);
        }
    }
    for (uint32_t i = 0; xs && ys && i < n; i++) {
        uint32_t j = (i + 1) % n;
        line(d, xs[i], ys[i], xs[j], ys[j]);
    }
    free(xs);
    free(ys);
    RC_RETURN(c, 1, 3);
}

/* ---- DCs and their state ---- */

static void k_CreateCompatibleDC(rc_cpu *c) { RC_RETURN(c, rc_dc_new(), 1); }

static void k_DeleteDC(rc_cpu *c)
{
    rc_dc_free(RC_ARG(c, 0));
    RC_RETURN(c, 1, 1);
}

static void k_SaveDC(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d || d->nsaved == 8) {
        RC_RETURN(c, 0, 1);
    }
    rc_dc_state *s = &d->saved[d->nsaved++];
    *s = (rc_dc_state){d->bitmap, d->font, d->pen, d->brush, d->text_color, d->bk_color, d->bk_mode, d->text_align,
                       d->rop2, d->map_mode, d->cur_x, d->cur_y, d->vorg_x, d->vorg_y, d->worg_x, d->worg_y,
                       d->has_clip, d->clip};
    RC_RETURN(c, (uint32_t)d->nsaved, 1);
}

/* RestoreDC(dc, n): n > 0 a level SaveDC returned, n < 0 that many back. */
static void k_RestoreDC(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    int32_t n = (int32_t)RC_ARG(c, 1);
    if (!d) {
        RC_RETURN(c, 0, 2);
    }
    int level = n < 0 ? d->nsaved + n : n - 1;
    if (level < 0 || level >= d->nsaved) {
        RC_RETURN(c, 0, 2);
    }
    rc_dc_state *s = &d->saved[level];
    if (s->bitmap != d->bitmap && s->bitmap) {
        rc_gdi_select(RC_ARG(c, 0), s->bitmap);
    }
    d->font = s->font;
    d->pen = s->pen;
    d->brush = s->brush;
    d->text_color = s->text_color;
    d->bk_color = s->bk_color;
    d->bk_mode = s->bk_mode;
    d->text_align = s->text_align;
    d->rop2 = s->rop2;
    d->map_mode = s->map_mode;
    d->cur_x = s->cur_x;
    d->cur_y = s->cur_y;
    d->vorg_x = s->vorg_x;
    d->vorg_y = s->vorg_y;
    d->worg_x = s->worg_x;
    d->worg_y = s->worg_y;
    d->has_clip = s->has_clip;
    d->clip = s->clip;
    d->nsaved = level;
    RC_RETURN(c, 1, 2);
}

#define SET_GET(fname, field, nargs_set)                                                                               \
    static void fname(rc_cpu *c)                                                                                       \
    {                                                                                                                  \
        rc_dc *d = rc_dc_get(RC_ARG(c, 0));                                                                            \
        uint32_t old = d ? d->field : 0;                                                                               \
        if (d && nargs_set == 2) {                                                                                     \
            d->field = RC_ARG(c, 1);                                                                                   \
        }                                                                                                              \
        RC_RETURN(c, old, nargs_set);                                                                                  \
    }

SET_GET(k_SetBkMode, bk_mode, 2)
SET_GET(k_GetBkMode, bk_mode, 1)
SET_GET(k_SetROP2, rop2, 2)
SET_GET(k_GetROP2, rop2, 1)
SET_GET(k_SetMapMode, map_mode, 2)
SET_GET(k_GetMapMode, map_mode, 1)
SET_GET(k_GetBkColor, bk_color, 1)
SET_GET(k_GetTextColor, text_color, 1)
SET_GET(k_GetTextAlign, text_align, 1)

static void put_point(uint32_t p, int32_t x, int32_t y)
{
    if (p) {
        rc_st32(p, (uint32_t)x);
        rc_st32(p + 4, (uint32_t)y);
    }
}

/* SetViewportOrgEx(dc, x, y, old*) and friends: origins only; extents are kept as 1:1. */
static void k_SetViewportOrgEx(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, 0, 4);
    }
    put_point(RC_ARG(c, 3), d->vorg_x, d->vorg_y);
    d->vorg_x = (int32_t)RC_ARG(c, 1);
    d->vorg_y = (int32_t)RC_ARG(c, 2);
    RC_RETURN(c, 1, 4);
}

static void k_OffsetViewportOrgEx(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, 0, 4);
    }
    put_point(RC_ARG(c, 3), d->vorg_x, d->vorg_y);
    d->vorg_x += (int32_t)RC_ARG(c, 1);
    d->vorg_y += (int32_t)RC_ARG(c, 2);
    RC_RETURN(c, 1, 4);
}

static void k_GetViewportOrgEx(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (d) {
        put_point(RC_ARG(c, 1), d->vorg_x, d->vorg_y);
    }
    RC_RETURN(c, d != NULL, 2);
}

static void k_SetWindowOrgEx(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, 0, 4);
    }
    put_point(RC_ARG(c, 3), d->worg_x, d->worg_y);
    d->worg_x = (int32_t)RC_ARG(c, 1);
    d->worg_y = (int32_t)RC_ARG(c, 2);
    RC_RETURN(c, 1, 4);
}

static void k_OffsetWindowOrgEx(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, 0, 4);
    }
    put_point(RC_ARG(c, 3), d->worg_x, d->worg_y);
    d->worg_x += (int32_t)RC_ARG(c, 1);
    d->worg_y += (int32_t)RC_ARG(c, 2);
    RC_RETURN(c, 1, 4);
}

static void k_GetWindowOrgEx(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (d) {
        put_point(RC_ARG(c, 1), d->worg_x, d->worg_y);
    }
    RC_RETURN(c, d != NULL, 2);
}

/* Extents (SetViewportExtEx, SetWindowExtEx, Scale...): 1:1 is kept; the old extent is (1, 1). */
static void k_ext4(rc_cpu *c)
{
    put_point(RC_ARG(c, 3), 1, 1);
    RC_RETURN(c, 1, 4);
}

static void k_ext6(rc_cpu *c)
{
    put_point(RC_ARG(c, 5), 1, 1);
    RC_RETURN(c, 1, 6);
}

static void k_GetExtEx(rc_cpu *c)
{
    put_point(RC_ARG(c, 1), 1, 1);
    RC_RETURN(c, 1, 2);
}

/* LPtoDP / DPtoLP(dc, points, n): the origins. */
static void convert_points(rc_cpu *c, int sign)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t p = RC_ARG(c, 1), n = RC_ARG(c, 2);
    if (!d) {
        RC_RETURN(c, 0, 3);
    }
    int32_t ox = d->vorg_x - d->worg_x, oy = d->vorg_y - d->worg_y;
    for (uint32_t i = 0; i < n; i++) {
        rc_st32(p + 8 * i, (uint32_t)((int32_t)rc_ld32(p + 8 * i) + sign * ox));
        rc_st32(p + 8 * i + 4, (uint32_t)((int32_t)rc_ld32(p + 8 * i + 4) + sign * oy));
    }
    RC_RETURN(c, 1, 3);
}

static void k_LPtoDP(rc_cpu *c) { convert_points(c, 1); }
static void k_DPtoLP(rc_cpu *c) { convert_points(c, -1); }

static void k_SetStretchBltMode(rc_cpu *c) { RC_RETURN(c, 1, 2); }
static void k_SetBrushOrgEx(rc_cpu *c)
{
    put_point(RC_ARG(c, 3), 0, 0);
    RC_RETURN(c, 1, 4);
}

static void k_GetNearestColor(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    RC_RETURN(c, d ? rc_dc_colorref(d, rc_dc_pixel(d, RC_ARG(c, 1) & 0xffffff)) : RC_ARG(c, 1), 2);
}

static void k_GetDCOrgEx(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (d) {
        put_point(RC_ARG(c, 1), d->org_x, d->org_y);
    }
    RC_RETURN(c, d != NULL, 2);
}

/* Escape(dc, code, size, in, out): no printer escapes. */
static void k_Escape(rc_cpu *c) { RC_RETURN(c, 0, 5); }

const rc_import_def rc_gdi32_draw[] = {
    {G32, "BitBlt", k_BitBlt},
    {G32, "StretchBlt", k_StretchBlt},
    {G32, "PatBlt", k_PatBlt},
    {G32, "StretchDIBits", k_StretchDIBits},
    {G32, "SetDIBitsToDevice", k_SetDIBitsToDevice},
    {G32, "MoveToEx", k_MoveToEx},
    {G32, "LineTo", k_LineTo},
    {G32, "GetCurrentPositionEx", k_GetCurrentPositionEx},
    {G32, "Rectangle", k_Rectangle},
    {G32, "RoundRect", k_RoundRect},
    {G32, "Ellipse", k_Ellipse},
    {G32, "Polyline", k_Polyline},
    {G32, "Polygon", k_Polygon},
    {G32, "CreateCompatibleDC", k_CreateCompatibleDC},
    {G32, "DeleteDC", k_DeleteDC},
    {G32, "SaveDC", k_SaveDC},
    {G32, "RestoreDC", k_RestoreDC},
    {G32, "SetBkMode", k_SetBkMode},
    {G32, "GetBkMode", k_GetBkMode},
    {G32, "GetBkColor", k_GetBkColor},
    {G32, "GetTextColor", k_GetTextColor},
    {G32, "GetTextAlign", k_GetTextAlign},
    {G32, "SetROP2", k_SetROP2},
    {G32, "GetROP2", k_GetROP2},
    {G32, "SetMapMode", k_SetMapMode},
    {G32, "GetMapMode", k_GetMapMode},
    {G32, "SetViewportOrgEx", k_SetViewportOrgEx},
    {G32, "OffsetViewportOrgEx", k_OffsetViewportOrgEx},
    {G32, "GetViewportOrgEx", k_GetViewportOrgEx},
    {G32, "SetWindowOrgEx", k_SetWindowOrgEx},
    {G32, "OffsetWindowOrgEx", k_OffsetWindowOrgEx},
    {G32, "GetWindowOrgEx", k_GetWindowOrgEx},
    {G32, "SetViewportExtEx", k_ext4},
    {G32, "SetWindowExtEx", k_ext4},
    {G32, "ScaleViewportExtEx", k_ext6},
    {G32, "ScaleWindowExtEx", k_ext6},
    {G32, "GetViewportExtEx", k_GetExtEx},
    {G32, "GetWindowExtEx", k_GetExtEx},
    {G32, "LPtoDP", k_LPtoDP},
    {G32, "DPtoLP", k_DPtoLP},
    {G32, "SetStretchBltMode", k_SetStretchBltMode},
    {G32, "SetBrushOrgEx", k_SetBrushOrgEx},
    {G32, "GetNearestColor", k_GetNearestColor},
    {G32, "GetDCOrgEx", k_GetDCOrgEx},
    {G32, "Escape", k_Escape},
};
const uint32_t rc_gdi32_draw_count = sizeof rc_gdi32_draw / sizeof rc_gdi32_draw[0];
