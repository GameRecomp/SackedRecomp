/* SPDX-License-Identifier: MIT */
/*
 * GDI's logical palettes, the background colour, and single pixels.
 *
 * A palette is a host table of up to 256 colours. Selecting one into a DC gives an 8-bit
 * target its colours (RealizePalette); with a 16- or 32-bit target it only matters for the
 * COLORREFs that name palette entries (PALETTEINDEX). SetPixel and GetPixel convert between
 * COLORREF and the target's pixel format.
 */
#include <stdlib.h>
#include <string.h>

#include "dc.h"
#include "gdi32.h"

#define G32 "GDI32.dll"
#define PALETTES 16
#define PALETTE_HANDLE 0x00060000u
#define CLR_INVALID 0xffffffffu

static struct {
    bool used;
    uint32_t n;
    uint32_t rgb[256]; /* 0x00RRGGBB */
} palettes[PALETTES];
static uint32_t selected[64]; /* hdc slot -> palette handle (by low bits of the hdc) */

static int palette_index(uint32_t h)
{
    if (h < PALETTE_HANDLE || (h - PALETTE_HANDLE) % 4 || (h - PALETTE_HANDLE) / 4 >= PALETTES) {
        return -1;
    }
    int i = (int)((h - PALETTE_HANDLE) / 4);
    return palettes[i].used ? i : -1;
}

bool rc_gdi_is_palette(uint32_t h) { return palette_index(h) >= 0; }

void rc_gdi_free_palette(uint32_t h)
{
    int i = palette_index(h);
    if (i >= 0) {
        palettes[i].used = false;
    }
}

/* CreatePalette(LOGPALETTE*): version, count, then PALETTEENTRY (r, g, b, flags) each. */
static void k_CreatePalette(rc_cpu *c)
{
    uint32_t lp = RC_ARG(c, 0);
    uint32_t n = rc_ld16(lp + 2);
    for (uint32_t i = 1; i < PALETTES; i++) {
        if (!palettes[i].used) {
            palettes[i].used = true;
            palettes[i].n = n > 256 ? 256 : n;
            for (uint32_t k = 0; k < palettes[i].n; k++) {
                uint32_t e = lp + 4 + 4 * k;
                palettes[i].rgb[k] = (uint32_t)rc_ld8(e) << 16 | (uint32_t)rc_ld8(e + 1) << 8 | rc_ld8(e + 2);
            }
            RC_RETURN(c, PALETTE_HANDLE + 4 * i, 1);
        }
    }
    RC_RETURN(c, 0, 1);
}

/* SelectPalette(hdc, palette, background): the palette selected before. */
static void k_SelectPalette(rc_cpu *c)
{
    uint32_t hdc = RC_ARG(c, 0), h = RC_ARG(c, 1);
    uint32_t *slot = &selected[hdc % 64];
    uint32_t old = *slot;
    *slot = h;
    RC_RETURN(c, old ? old : PALETTE_HANDLE /* the default palette */, 3);
}

/* RealizePalette(hdc): the entries of the selected palette become an 8-bit target's colours. */
static void k_RealizePalette(rc_cpu *c)
{
    uint32_t hdc = RC_ARG(c, 0);
    int i = palette_index(selected[hdc % 64]);
    rc_dc *d = rc_dc_get(hdc);
    if (i < 0 || !d) {
        RC_RETURN(c, 0, 1);
    }
    for (uint32_t k = 0; k < palettes[i].n; k++) {
        d->palette[k] = palettes[i].rgb[k];
    }
    d->has_palette = 1;
    RC_RETURN(c, palettes[i].n, 1);
}

static void k_SetBkColor(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, CLR_INVALID, 2);
    }
    uint32_t old = d->bk_color;
    d->bk_color = RC_ARG(c, 1);
    RC_RETURN(c, old, 2);
}

static uint32_t channel(uint32_t v, uint32_t mask)
{
    if (!mask) {
        return 0;
    }
    int shift = 0, bits = 0;
    while (!((mask >> shift) & 1)) {
        shift++;
    }
    while ((mask >> (shift + bits)) & 1) {
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

/* The COLORREF 0x00BBGGRR (or a PALETTEINDEX 0x01iiiiii) as the target's pixel. */
static uint32_t to_pixel(const rc_dc *d, uint32_t ref, uint32_t hdc)
{
    if ((ref >> 24) == 1) {
        int i = palette_index(selected[hdc % 64]);
        uint32_t k = ref & 0xffff;
        uint32_t rgb = i >= 0 && k < palettes[i].n ? palettes[i].rgb[k] : 0;
        ref = (rgb >> 16 & 0xff) | (rgb & 0xff00) | (rgb & 0xff) << 16;
        if (d->target.bpp == 8) {
            return k;
        }
    }
    uint32_t r = ref & 0xff, g = ref >> 8 & 0xff, b = ref >> 16 & 0xff;
    if (d->target.bpp == 8) {
        /* The nearest colour of the DC's palette. */
        uint32_t best = 0, dist = 0xffffffffu;
        for (uint32_t k = 0; k < 256; k++) {
            uint32_t q = d->palette[k];
            int dr = (int)(q >> 16 & 0xff) - (int)r, dg = (int)(q >> 8 & 0xff) - (int)g, db = (int)(q & 0xff) - (int)b;
            uint32_t e = (uint32_t)(dr * dr + dg * dg + db * db);
            if (e < dist) {
                dist = e;
                best = k;
            }
        }
        return best;
    }
    uint32_t rm = d->target.rmask, gm = d->target.gmask, bm = d->target.bmask;
    if (!rm && !gm && !bm) {
        if (d->target.bpp == 16) {
            rm = 0xf800, gm = 0x07e0, bm = 0x001f;
        } else {
            rm = 0xff0000, gm = 0xff00, bm = 0xff;
        }
    }
    return channel(r, rm) | channel(g, gm) | channel(b, bm);
}

static uint32_t to_ref(const rc_dc *d, uint32_t p)
{
    if (d->target.bpp == 8) {
        uint32_t q = d->palette[p & 0xff];
        return (q >> 16 & 0xff) | (q & 0xff00) | (q & 0xff) << 16;
    }
    uint32_t rm = d->target.rmask, gm = d->target.gmask, bm = d->target.bmask;
    if (!rm && !gm && !bm) {
        if (d->target.bpp == 16) {
            rm = 0xf800, gm = 0x07e0, bm = 0x001f;
        } else {
            rm = 0xff0000, gm = 0xff00, bm = 0xff;
        }
    }
    return unchannel(p, rm) | unchannel(p, gm) << 8 | unchannel(p, bm) << 16;
}

/* The address of pixel (x, y) (logical) of the DC's target, or 0 outside where it draws. */
static uint32_t pixel_at(const rc_dc *d, int32_t x, int32_t y)
{
    const rc_pixels *t = &d->target;
    int32_t dx = rc_dc_x(d, x), dy = rc_dc_y(d, y);
    x = dx + d->org_x;
    y = dy + d->org_y;
    if (!t->pixels || x < 0 || y < 0 || (uint32_t)x >= t->w || (uint32_t)y >= t->h || t->bpp < 8) {
        return 0;
    }
    if ((d->vis.n >= 0 && !rc_rgn_has(&d->vis, x, y)) || (d->has_clip && !rc_rgn_has(&d->clip, dx, dy))) {
        return 0;
    }
    return t->pixels + (uint32_t)y * t->pitch + (uint32_t)x * (t->bpp / 8);
}

/* SetPixel(hdc, x, y, color): the colour it set. */
static void k_SetPixel(rc_cpu *c)
{
    uint32_t hdc = RC_ARG(c, 0);
    rc_dc *d = rc_dc_get(hdc);
    uint32_t a = d ? pixel_at(d, (int32_t)RC_ARG(c, 1), (int32_t)RC_ARG(c, 2)) : 0;
    if (!a) {
        RC_RETURN(c, CLR_INVALID, 4);
    }
    uint32_t p = to_pixel(d, RC_ARG(c, 3), hdc);
    switch (d->target.bpp) {
    case 8: rc_st8(a, (uint8_t)p); break;
    case 16: rc_st16(a, (uint16_t)p); break;
    case 24: rc_st16(a, (uint16_t)p); rc_st8(a + 2, (uint8_t)(p >> 16)); break;
    default: rc_st32(a, p); break;
    }
    RC_RETURN(c, to_ref(d, p), 4);
}

static void k_GetPixel(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t a = d ? pixel_at(d, (int32_t)RC_ARG(c, 1), (int32_t)RC_ARG(c, 2)) : 0;
    if (!a) {
        RC_RETURN(c, CLR_INVALID, 3);
    }
    uint32_t p;
    switch (d->target.bpp) {
    case 8: p = rc_ld8(a); break;
    case 16: p = rc_ld16(a); break;
    case 24: p = rc_ld16(a) | (uint32_t)rc_ld8(a + 2) << 16; break;
    default: p = rc_ld32(a); break;
    }
    RC_RETURN(c, to_ref(d, p), 3);
}

const rc_import_def rc_gdi32_palette[] = {
    {G32, "CreatePalette", k_CreatePalette},
    {G32, "SelectPalette", k_SelectPalette},
    {G32, "RealizePalette", k_RealizePalette},
    {G32, "SetBkColor", k_SetBkColor},
    {G32, "SetPixel", k_SetPixel},
    {G32, "GetPixel", k_GetPixel},
};
const uint32_t rc_gdi32_palette_count = sizeof rc_gdi32_palette / sizeof rc_gdi32_palette[0];
