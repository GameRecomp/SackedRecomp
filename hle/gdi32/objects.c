/* SPDX-License-Identifier: MIT */
/*
 * gdi32: the object table (bitmaps, fonts, pens, brushes, regions), stock objects, creating
 * bitmaps (device-dependent ones at 32 bits, DIB sections as asked), brushes and pens, and
 * SelectObject, DeleteObject, GetObjectA.
 */
#include <stdio.h>
#include <string.h>

#include "dc.h"
#include "gdi32.h"
#include "objects.h"
#include "rc/hle.h"
#include "rc/log.h"
#include "rc/vm.h"

#define G32 "GDI32.dll"
#define OBJS 8192
#define OBJ_HANDLE 0x03000000u

static gobj objs[OBJS];

uint32_t rc_gobj_new(enum okind k)
{
    for (uint32_t i = 64; i < OBJS; i++) { /* the first 64 are the stock objects */
        if (objs[i].kind == OB_FREE) {
            memset(&objs[i], 0, sizeof objs[i]);
            objs[i].kind = k;
            return OBJ_HANDLE + 4 * i;
        }
    }
    rc_fatal("gdi32: out of objects");
}

static gobj *slot(uint32_t h)
{
    if (h < OBJ_HANDLE || (h - OBJ_HANDLE) % 4 || (h - OBJ_HANDLE) / 4 >= OBJS) {
        return NULL;
    }
    return &objs[(h - OBJ_HANDLE) / 4];
}

gobj *rc_gobj_get(uint32_t h, enum okind k)
{
    rc_gdi_stock(0); /* the stock objects exist */
    gobj *o = slot(h);
    return o && o->kind == k ? o : NULL;
}

enum okind rc_gobj_kind(uint32_t h)
{
    rc_gdi_stock(0);
    gobj *o = slot(h);
    return o ? o->kind : OB_FREE;
}

void rc_gobj_free(uint32_t h)
{
    gobj *o = slot(h);
    if (!o || o->stock || o->kind == OB_FREE) {
        return;
    }
    if (o->kind == OB_BITMAP && o->own_pixels && o->pixels) {
        rc_heap_free(o->pixels);
    }
    o->kind = OB_FREE;
}

/* ---- stock objects ---- */

static void stock_brush(uint32_t i, uint32_t color, uint32_t style)
{
    objs[i] = (gobj){.kind = OB_BRUSH, .stock = true, .color = color, .style = style};
}

static void stock_pen(uint32_t i, uint32_t color, uint32_t style)
{
    objs[i] = (gobj){.kind = OB_PEN, .stock = true, .color = color, .style = style, .pwidth = 1};
}

static void stock_font(uint32_t i, const char *face, int32_t height, uint32_t weight, uint32_t pitch)
{
    objs[i] = (gobj){.kind = OB_FONT, .stock = true, .height = height, .weight = weight, .pitch = pitch};
    snprintf(objs[i].face, sizeof objs[i].face, "%s", face);
}

uint32_t rc_gdi_stock(uint32_t index)
{
    static bool made;
    if (!made) {
        made = true;
        stock_brush(0, 0xffffff, 0);         /* WHITE_BRUSH */
        stock_brush(1, 0xc0c0c0, 0);         /* LTGRAY_BRUSH */
        stock_brush(2, 0x808080, 0);         /* GRAY_BRUSH */
        stock_brush(3, 0x404040, 0);         /* DKGRAY_BRUSH */
        stock_brush(4, 0x000000, 0);         /* BLACK_BRUSH */
        stock_brush(5, 0, 1);                /* NULL_BRUSH (BS_NULL) */
        stock_pen(6, 0xffffff, 0);           /* WHITE_PEN */
        stock_pen(7, 0x000000, 0);           /* BLACK_PEN */
        stock_pen(8, 0, 5);                  /* NULL_PEN */
        stock_font(10, "Terminal", 12, 400, 1);      /* OEM_FIXED_FONT */
        stock_font(11, "Courier New", -13, 400, 1);  /* ANSI_FIXED_FONT */
        stock_font(12, "MS Sans Serif", -11, 400, 2); /* ANSI_VAR_FONT */
        stock_font(13, "System", 16, 700, 2);        /* SYSTEM_FONT */
        stock_font(14, "System", 16, 700, 2);        /* DEVICE_DEFAULT_FONT */
        stock_font(16, "Fixedsys", 15, 400, 1);      /* SYSTEM_FIXED_FONT */
        stock_font(17, "MS Sans Serif", -11, 400, 2); /* DEFAULT_GUI_FONT */
        /* 15 is DEFAULT_PALETTE (palette.c); 18 DC_BRUSH and 19 DC_PEN behave as white and black. */
        stock_brush(18, 0xffffff, 0);
        stock_pen(19, 0x000000, 0);
        /* 20: the default bitmap of a memory DC, 1x1 monochrome. */
        objs[20] = (gobj){.kind = OB_BITMAP, .stock = true, .w = 1, .h = 1, .bpp = 1, .widthbytes = 2};
    }
    if (index == 15) {
        return 0x05000000u; /* palette.c's default palette */
    }
    return index <= 20 && objs[index].kind != OB_FREE ? OBJ_HANDLE + 4 * index : 0;
}

static void k_GetStockObject(rc_cpu *c) { RC_RETURN(c, rc_gdi_stock(RC_ARG(c, 0)), 1); }

/* ---- bitmaps ---- */

void rc_gdi_bitmap_pixels(const gobj *b, rc_pixels *out)
{
    uint32_t r = 0, g = 0, bl = 0;
    if (b->bpp == 16) {
        r = b->masks[0] ? b->masks[0] : 0x7c00;
        g = b->masks[1] ? b->masks[1] : 0x03e0;
        bl = b->masks[2] ? b->masks[2] : 0x001f;
        if (!b->dib) {
            r = 0xf800, g = 0x07e0, bl = 0x001f;
        }
    } else if (b->bpp >= 24) {
        r = 0xff0000, g = 0xff00, bl = 0xff;
    }
    uint32_t base = b->pixels, pitch = b->widthbytes;
    if (b->bottom_up && b->h) {
        base = b->pixels + (b->h - 1) * b->widthbytes;
        pitch = (uint32_t)-(int32_t)b->widthbytes; /* rows upwards: addresses wrap round */
    }
    *out = (rc_pixels){base, b->w, b->h, pitch, b->bpp, r, g, bl};
}

bool rc_gdi_bitmap(uint32_t h, uint32_t *width, uint32_t *height, uint32_t *bpp, uint32_t *widthbytes,
                   uint32_t *pixels)
{
    gobj *b = rc_gobj_get(h, OB_BITMAP);
    if (!b) {
        return false;
    }
    *width = b->w;
    *height = b->h;
    *bpp = b->bpp;
    *widthbytes = b->widthbytes;
    *pixels = b->pixels;
    return true;
}

static uint32_t new_bitmap(uint32_t w, uint32_t h, uint32_t bpp, uint32_t bits)
{
    if (w == 0 || h == 0 || w > 16384 || h > 16384) {
        return 0;
    }
    uint32_t hb = rc_gobj_new(OB_BITMAP);
    gobj *b = rc_gobj_get(hb, OB_BITMAP);
    b->w = w;
    b->h = h;
    b->bpp = bpp;
    b->widthbytes = ((w * bpp + 15) / 16) * 2; /* rows are word aligned */
    b->pixels = rc_heap_alloc(b->widthbytes * h, true);
    b->own_pixels = true;
    if (bits) {
        memcpy(rc_ptr(b->pixels), rc_ptr(bits), (size_t)b->widthbytes * h);
    }
    return hb;
}

static void k_CreateBitmapIndirect(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    uint32_t w = rc_ld32(p + 4), h = rc_ld32(p + 8), bpp = rc_ld16(p + 18), bits = rc_ld32(p + 20);
    uint32_t hb = new_bitmap(w, h, bpp, 0);
    gobj *b = rc_gobj_get(hb, OB_BITMAP);
    if (b && bits) {
        uint32_t wb = rc_ld32(p + 12) > b->widthbytes ? rc_ld32(p + 12) : b->widthbytes;
        for (uint32_t y = 0; y < h; y++) {
            memcpy(rc_ptr(b->pixels + y * b->widthbytes), rc_ptr(bits + y * wb), b->widthbytes);
        }
    }
    RC_RETURN(c, hb, 1);
}

/* CreateBitmap(w, h, planes, bpp, bits) */
static void k_CreateBitmap(rc_cpu *c)
{
    uint32_t bpp = RC_ARG(c, 3) * (RC_ARG(c, 2) ? RC_ARG(c, 2) : 1);
    RC_RETURN(c, new_bitmap(RC_ARG(c, 0), RC_ARG(c, 1), bpp, RC_ARG(c, 4)), 5);
}

/* CreateCompatibleBitmap(dc, w, h): as the DC's pixels (a memory DC without a bitmap of its own
 * is monochrome); the screen is 32 bits. */
static void k_CreateCompatibleBitmap(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t bpp = d && d->target.pixels ? d->target.bpp : 32;
    if (d && !d->hwnd && !d->target.pixels) {
        bpp = 1;
    }
    if (bpp == 8 || bpp == 24) {
        bpp = 32;
    }
    RC_RETURN(c, new_bitmap(RC_ARG(c, 1) ? RC_ARG(c, 1) : 1, RC_ARG(c, 2) ? RC_ARG(c, 2) : 1, bpp, 0), 3);
}

/* A BITMAPINFO's color table into `b` (0x00RRGGBB entries); DIB_PAL_COLORS gives gray. */
static void read_colors(gobj *b, uint32_t bmi, uint32_t usage)
{
    uint32_t hsize = rc_ld32(bmi), bpp = rc_ld16(bmi + 14), comp = rc_ld32(bmi + 16), used = rc_ld32(bmi + 32);
    if (bpp <= 8) {
        uint32_t n = used ? used : 1u << bpp;
        if (n > 256) {
            n = 256;
        }
        b->ncolors = n;
        for (uint32_t i = 0; i < n; i++) {
            if (usage == 1) {
                uint32_t k = rc_ld16(bmi + hsize + 2 * i) & 0xff;
                b->colors[i] = k * 0x010101u;
            } else {
                b->colors[i] = rc_ld32(bmi + hsize + 4 * i) & 0xffffff; /* RGBQUAD: B, G, R, 0 */
            }
        }
    } else if (comp == 3 /* BI_BITFIELDS */) {
        for (int i = 0; i < 3; i++) {
            b->masks[i] = rc_ld32(bmi + 40 + 4 * (uint32_t)i); /* after the header, or in a V4/V5 one */
        }
    }
}

/* CreateDIBSection(dc, bmi, usage, bits*, section, offset) */
static void k_CreateDIBSection(rc_cpu *c)
{
    uint32_t bmi = RC_ARG(c, 1), usage = RC_ARG(c, 2), pbits = RC_ARG(c, 3);
    int32_t w = (int32_t)rc_ld32(bmi + 4), h = (int32_t)rc_ld32(bmi + 8);
    uint32_t bpp = rc_ld16(bmi + 14);
    if (w <= 0 || h == 0 || bpp == 0) {
        RC_RETURN(c, 0, 6);
    }
    uint32_t ah = (uint32_t)(h < 0 ? -h : h);
    uint32_t hb = rc_gobj_new(OB_BITMAP);
    gobj *b = rc_gobj_get(hb, OB_BITMAP);
    b->w = (uint32_t)w;
    b->h = ah;
    b->bpp = bpp;
    b->dib = true;
    b->bottom_up = h > 0;
    b->widthbytes = (((uint32_t)w * bpp + 31) / 32) * 4; /* DIB rows are dword aligned */
    b->pixels = rc_heap_alloc(b->widthbytes * ah, true);
    b->own_pixels = true;
    read_colors(b, bmi, usage);
    if (pbits) {
        rc_st32(pbits, b->pixels);
    }
    if (rc_trace("gdi")) {
        rc_log("[gdi] DIB section %dx%d, %u bits", w, h, bpp);
    }
    RC_RETURN(c, hb, 6);
}

/* A 32-bit bitmap with the picture of a DIB (resources, LoadBitmap). */
uint32_t rc_gdi_bitmap_from_dib(uint32_t bmi, uint32_t bits)
{
    int32_t w = (int32_t)rc_ld32(bmi + 4), h = (int32_t)rc_ld32(bmi + 8);
    uint32_t hb = new_bitmap((uint32_t)w, (uint32_t)(h < 0 ? -h : h), 32, 0);
    if (hb) {
        rc_gdi_draw_dib_into(hb, bits, bmi, 0);
    }
    return hb;
}

/* CreateDIBitmap(dc, header, init, bits, bmi, usage): a 32-bit bitmap, filled from the DIB with
 * CBM_INIT (4). */
static void k_CreateDIBitmap(rc_cpu *c)
{
    uint32_t hdr = RC_ARG(c, 1), init = RC_ARG(c, 2), bits = RC_ARG(c, 3), bmi = RC_ARG(c, 4), usage = RC_ARG(c, 5);
    int32_t w = (int32_t)rc_ld32(hdr + 4), h = (int32_t)rc_ld32(hdr + 8);
    uint32_t hb = new_bitmap((uint32_t)w, (uint32_t)(h < 0 ? -h : h), 32, 0);
    if (hb && (init & 4) && bits && bmi) {
        rc_gdi_draw_dib_into(hb, bits, bmi, usage);
    }
    RC_RETURN(c, hb, 6);
}

/* SetDIBColorTable(dc, start, count, colors): of the DIB section selected into the DC. */
static void k_SetDIBColorTable(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    gobj *b = d ? rc_gobj_get(d->bitmap, OB_BITMAP) : NULL;
    uint32_t start = RC_ARG(c, 1), n = RC_ARG(c, 2), p = RC_ARG(c, 3);
    if (!b || !b->dib || b->bpp > 8) {
        RC_RETURN(c, 0, 4);
    }
    uint32_t k = 0;
    for (; k < n && start + k < 256; k++) {
        uint32_t q = rc_ld32(p + 4 * k) & 0xffffff;
        b->colors[start + k] = q;
        d->palette[start + k] = q;
    }
    if (b->ncolors < start + k) {
        b->ncolors = start + k;
    }
    d->has_palette = 1;
    RC_RETURN(c, k, 4);
}

static void k_GetBitmapBits(rc_cpu *c)
{
    gobj *b = rc_gobj_get(RC_ARG(c, 0), OB_BITMAP);
    uint32_t n = RC_ARG(c, 1), out = RC_ARG(c, 2);
    if (!b || !b->pixels) {
        RC_RETURN(c, 0, 3);
    }
    uint32_t size = b->widthbytes * b->h;
    if (n > size) {
        n = size;
    }
    memcpy(rc_ptr(out), rc_ptr(b->pixels), n);
    RC_RETURN(c, n, 3);
}

/* ---- brushes and pens ---- */

static uint32_t new_brush(uint32_t style, uint32_t color, uint32_t hatch, uint32_t pattern)
{
    uint32_t h = rc_gobj_new(OB_BRUSH);
    gobj *b = rc_gobj_get(h, OB_BRUSH);
    b->style = style;
    b->color = color & 0x00ffffffu;
    b->hatch = hatch;
    b->pattern = pattern;
    return h;
}

uint32_t rc_gdi_solid_brush(uint32_t color) { return new_brush(0, color, 0, 0); }

static void k_CreateSolidBrush(rc_cpu *c) { RC_RETURN(c, new_brush(0, RC_ARG(c, 0), 0, 0), 1); }
static void k_CreateHatchBrush(rc_cpu *c) { RC_RETURN(c, new_brush(2, RC_ARG(c, 1), RC_ARG(c, 0), 0), 2); }

/* CreatePatternBrush(bitmap): the brush keeps its own copy of the pattern. */
static void k_CreatePatternBrush(rc_cpu *c)
{
    gobj *src = rc_gobj_get(RC_ARG(c, 0), OB_BITMAP);
    if (!src) {
        RC_RETURN(c, 0, 1);
    }
    uint32_t copy = new_bitmap(src->w, src->h, src->bpp, 0);
    gobj *b = rc_gobj_get(copy, OB_BITMAP);
    if (b) {
        uint32_t wb = src->widthbytes < b->widthbytes ? src->widthbytes : b->widthbytes;
        rc_pixels sp;
        rc_gdi_bitmap_pixels(src, &sp);
        for (uint32_t y = 0; y < src->h; y++) {
            memcpy(rc_ptr(b->pixels + y * b->widthbytes), rc_ptr(sp.pixels + y * sp.pitch), wb);
        }
        memcpy(b->colors, src->colors, sizeof b->colors);
        b->ncolors = src->ncolors;
    }
    RC_RETURN(c, new_brush(3 /* BS_PATTERN */, 0, 0, copy), 1);
}

/* CreateBrushIndirect(LOGBRUSH: style, color, hatch) */
static void k_CreateBrushIndirect(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0), style = rc_ld32(p);
    RC_RETURN(c, new_brush(style, rc_ld32(p + 4), rc_ld32(p + 8), style == 3 ? rc_ld32(p + 8) : 0), 1);
}

/* PS_SOLID 0, PS_DASH 1, PS_DOT 2, PS_DASHDOT 3, PS_DASHDOTDOT 4, PS_NULL 5, PS_INSIDEFRAME 6;
 * styles are drawn solid. */
static void k_CreatePen(rc_cpu *c)
{
    uint32_t h = rc_gobj_new(OB_PEN);
    gobj *p = rc_gobj_get(h, OB_PEN);
    p->style = RC_ARG(c, 0) & 0xf;
    p->pwidth = RC_ARG(c, 1) ? RC_ARG(c, 1) : 1;
    p->color = RC_ARG(c, 2) & 0x00ffffffu;
    RC_RETURN(c, h, 3);
}

/* CreatePenIndirect(LOGPEN: style, width (POINT), color) */
static void k_CreatePenIndirect(rc_cpu *c)
{
    uint32_t q = RC_ARG(c, 0);
    uint32_t h = rc_gobj_new(OB_PEN);
    gobj *p = rc_gobj_get(h, OB_PEN);
    p->style = rc_ld32(q) & 0xf;
    p->pwidth = rc_ld32(q + 4) ? rc_ld32(q + 4) : 1;
    p->color = rc_ld32(q + 12) & 0x00ffffffu;
    RC_RETURN(c, h, 1);
}

/* ---- selecting, deleting, describing ---- */

uint32_t rc_gdi_select(uint32_t hdc, uint32_t h)
{
    rc_dc *d = rc_dc_get(hdc);
    if (!d) {
        return 0;
    }
    switch (rc_gobj_kind(h)) {
    case OB_BITMAP: {
        gobj *b = rc_gobj_get(h, OB_BITMAP);
        uint32_t old = d->bitmap ? d->bitmap : rc_gdi_stock(20);
        d->bitmap = h;
        if (b->pixels) {
            rc_gdi_bitmap_pixels(b, &d->target);
        } else {
            d->target = (rc_pixels){0};
        }
        if (b->bpp <= 8 && b->ncolors) {
            for (uint32_t i = 0; i < 256; i++) {
                d->palette[i] = i < b->ncolors ? b->colors[i] : 0;
            }
            d->has_palette = 1;
        }
        d->vis.n = -1;
        return old;
    }
    case OB_FONT: {
        uint32_t old = d->font ? d->font : rc_gdi_stock(13);
        d->font = h;
        return old;
    }
    case OB_PEN: {
        uint32_t old = d->pen ? d->pen : rc_gdi_stock(7);
        d->pen = h;
        return old;
    }
    case OB_BRUSH: {
        uint32_t old = d->brush ? d->brush : rc_gdi_stock(0);
        d->brush = h;
        return old;
    }
    case OB_REGION: {
        gobj *o = rc_gobj_get(h, OB_REGION);
        d->has_clip = true;
        d->clip = o->rgn;
        return o->rgn.n == 0 ? 1 : o->rgn.n == 1 ? 2 : 3;
    }
    default:
        return 0;
    }
}

static void k_SelectObject(rc_cpu *c) { RC_RETURN(c, rc_gdi_select(RC_ARG(c, 0), RC_ARG(c, 1)), 2); }

static void k_DeleteObject(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0);
    if (rc_gdi_is_palette(h)) {
        rc_gdi_free_palette(h);
        RC_RETURN(c, 1, 1);
    }
    gobj *o = slot(h);
    if (!o || o->kind == OB_FREE) {
        RC_RETURN(c, 0, 1);
    }
    if (o->kind == OB_BRUSH && o->pattern) {
        rc_gobj_free(o->pattern);
    }
    rc_gobj_free(h);
    RC_RETURN(c, 1, 1);
}

/* GetObjectA(object, size, buffer): BITMAP (24 bytes, DIBSECTION 84), LOGFONTA (60), LOGPEN (16),
 * LOGBRUSH (12). Without a buffer, the size. */
static void k_GetObjectA(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0), n = RC_ARG(c, 1), out = RC_ARG(c, 2);
    uint8_t buf[84] = {0};
    uint32_t size = 0;
    gobj *o = slot(h);
    if (rc_gdi_is_palette(h)) {
        if (out && n >= 2) {
            rc_st16(out, 256);
        }
        RC_RETURN(c, 2, 3);
    }
    if (!o || o->kind == OB_FREE) {
        RC_RETURN(c, 0, 3);
    }
    switch (o->kind) {
    case OB_BITMAP: {
        uint32_t v[4] = {0, o->w, o->h, o->widthbytes};
        memcpy(buf, v, 16);
        uint16_t planes = 1, bpp = (uint16_t)o->bpp;
        memcpy(buf + 16, &planes, 2);
        memcpy(buf + 18, &bpp, 2);
        uint32_t bits = o->dib ? o->pixels : 0;
        memcpy(buf + 20, &bits, 4);
        size = 24;
        if (o->dib && n >= 84) { /* DIBSECTION: + BITMAPINFOHEADER, masks, section, offset */
            uint32_t hdr[10] = {40, o->w, o->bottom_up ? o->h : (uint32_t)-(int32_t)o->h, 1u | (o->bpp << 16), 0,
                                o->widthbytes * o->h, 0, 0, o->ncolors, 0};
            memcpy(buf + 24, hdr, 40);
            memcpy(buf + 64, o->masks, 12);
            size = 84;
        }
        break;
    }
    case OB_FONT: {
        int32_t v[5] = {o->height, o->width, 0, 0, (int32_t)o->weight};
        memcpy(buf, v, 20);
        buf[20] = (uint8_t)o->italic;
        buf[21] = (uint8_t)o->underline;
        buf[23] = (uint8_t)o->charset;
        buf[27] = (uint8_t)o->pitch;
        snprintf((char *)buf + 28, 32, "%.31s", o->face);
        size = 60;
        break;
    }
    case OB_PEN: {
        uint32_t v[4] = {o->style, o->pwidth, 0, o->color};
        memcpy(buf, v, 16);
        size = 16;
        break;
    }
    case OB_BRUSH: {
        uint32_t v[3] = {o->style, o->color, o->style == 3 ? o->pattern : o->hatch};
        memcpy(buf, v, 12);
        size = 12;
        break;
    }
    default:
        RC_RETURN(c, 0, 3);
    }
    if (!out) {
        RC_RETURN(c, size, 3);
    }
    if (n < size) {
        size = n;
    }
    memcpy(rc_ptr(out), buf, size);
    RC_RETURN(c, size, 3);
}

/* GetObjectType(object): OBJ_PEN 1, BRUSH 2, DC 3, PAL 5, FONT 6, BITMAP 7, REGION 8, MEMDC 10. */
static void k_GetObjectType(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0);
    static const uint32_t types[] = {0, 7, 6, 1, 2, 8};
    if (rc_dc_get(h)) {
        RC_RETURN(c, rc_dc_get(h)->hwnd ? 3 : 10, 1);
    }
    if (rc_gdi_is_palette(h)) {
        RC_RETURN(c, 5, 1);
    }
    RC_RETURN(c, types[rc_gobj_kind(h)], 1);
}

/* GetCurrentObject(dc, type) */
static void k_GetCurrentObject(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, 0, 2);
    }
    switch (RC_ARG(c, 1)) {
    case 1: RC_RETURN(c, d->pen ? d->pen : rc_gdi_stock(7), 2);
    case 2: RC_RETURN(c, d->brush ? d->brush : rc_gdi_stock(0), 2);
    case 6: RC_RETURN(c, d->font ? d->font : rc_gdi_stock(13), 2);
    case 7: RC_RETURN(c, d->bitmap ? d->bitmap : rc_gdi_stock(20), 2);
    default: RC_RETURN(c, 0, 2);
    }
}

const rc_import_def rc_gdi32_objects[] = {
    {G32, "GetStockObject", k_GetStockObject},
    {G32, "CreateBitmapIndirect", k_CreateBitmapIndirect},
    {G32, "CreateBitmap", k_CreateBitmap},
    {G32, "CreateCompatibleBitmap", k_CreateCompatibleBitmap},
    {G32, "CreateDIBSection", k_CreateDIBSection},
    {G32, "CreateDIBitmap", k_CreateDIBitmap},
    {G32, "SetDIBColorTable", k_SetDIBColorTable},
    {G32, "GetBitmapBits", k_GetBitmapBits},
    {G32, "CreateSolidBrush", k_CreateSolidBrush},
    {G32, "CreateHatchBrush", k_CreateHatchBrush},
    {G32, "CreatePatternBrush", k_CreatePatternBrush},
    {G32, "CreateBrushIndirect", k_CreateBrushIndirect},
    {G32, "CreatePen", k_CreatePen},
    {G32, "CreatePenIndirect", k_CreatePenIndirect},
    {G32, "SelectObject", k_SelectObject},
    {G32, "DeleteObject", k_DeleteObject},
    {G32, "GetObjectA", k_GetObjectA},
    {G32, "GetObjectType", k_GetObjectType},
    {G32, "GetCurrentObject", k_GetCurrentObject},
};
const uint32_t rc_gdi32_objects_count = sizeof rc_gdi32_objects / sizeof rc_gdi32_objects[0];
