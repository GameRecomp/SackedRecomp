/* SPDX-License-Identifier: MIT */
/*
 * gdi32: fonts and text. Text is rasterised by the platform's fonts (see rc/text.h) without
 * antialiasing, in the DC's text color, over its background color when the background mode is
 * OPAQUE (the default), and drawn through the clipping of draw.c.
 */
#include <stdio.h>
#include <string.h>

#include "dc.h"
#include "gdi32.h"
#include "objects.h"
#include "rc/codepage.h"
#include "rc/hle.h"
#include "rc/hostfs.h"
#include "rc/log.h"
#include "rc/text.h"
#include "rc/vm.h"
#include "kernel32/k32.h"

#define G32 "GDI32.dll"

/* ---- fonts ---- */

static uint32_t new_font(int32_t height, int32_t width, uint32_t weight, uint32_t italic, uint32_t underline,
                         uint32_t charset, uint32_t pitch, const char *face)
{
    uint32_t hf = rc_gobj_new(OB_FONT);
    gobj *f = rc_gobj_get(hf, OB_FONT);
    f->height = height;
    f->width = width;
    f->weight = weight ? weight : 400;
    f->italic = italic;
    f->underline = underline;
    f->charset = charset;
    f->pitch = pitch;
    snprintf(f->face, sizeof f->face, "%s", face && *face ? face : "Arial");
    if (rc_trace("gdi")) {
        rc_log("[gdi] font \"%s\" height %d weight %u%s", f->face, f->height, f->weight, f->italic ? " italic" : "");
    }
    return hf;
}

/* CreateFontA(height, width, escapement, orientation, weight, italic, underline, strikeout,
 * charset, precision, clip precision, quality, pitch and family, face) */
static void k_CreateFontA(rc_cpu *c)
{
    const char *face = RC_ARG(c, 13) ? rc_str(RC_ARG(c, 13)) : NULL;
    RC_RETURN(c,
              new_font((int32_t)RC_ARG(c, 0), (int32_t)RC_ARG(c, 1), RC_ARG(c, 4), RC_ARG(c, 5) & 0xff,
                       RC_ARG(c, 6) & 0xff, RC_ARG(c, 8) & 0xff, RC_ARG(c, 12) & 0xff, face),
              14);
}

/* AddFontResourceA(file): the program's own font, found by its family name from then on;
 * the number of fonts added. */
static void k_AddFontResourceA(rc_cpu *c)
{
    char rel[512], host[1024];
    const char *path = rc_str(RC_ARG(c, 0));
    uint32_t size = 0;
    uint8_t *bytes = NULL;
    if (path && rc_k32_guest_rel(path, rel, sizeof rel) && rc_k32_find_existing(rel, host, NULL, NULL)) {
        bytes = rc_host_read(host, &size); /* (in a browser the game's files are read only this way) */
    }
    if (!bytes || !rc_font_add_memory(bytes, size)) {
        rc_log("[gdi] AddFontResourceA(\"%s\"): not added", path ? path : "");
        RC_RETURN(c, 0, 1);
    }
    RC_RETURN(c, 1, 1);
}

/* CreateFontIndirectA(LOGFONTA) */
static void k_CreateFontIndirectA(rc_cpu *c)
{
    uint32_t lf = RC_ARG(c, 0);
    char face[33];
    snprintf(face, sizeof face, "%.32s", (const char *)rc_ptr(lf + 28));
    RC_RETURN(c,
              new_font((int32_t)rc_ld32(lf), (int32_t)rc_ld32(lf + 4), rc_ld32(lf + 16), rc_ld8(lf + 20),
                       rc_ld8(lf + 21), rc_ld8(lf + 23), rc_ld8(lf + 27), face),
              1);
}

rc_font *rc_gdi_font_of(const rc_dc *d)
{
    gobj *f = rc_gobj_get(d->font ? d->font : rc_gdi_stock(13), OB_FONT);
    if (f) {
        rc_font *r = rc_font_get(f->face, f->height, (int)f->weight, f->italic != 0);
        if (r) {
            return r;
        }
    }
    return rc_font_get("System", 16, 700, false);
}

static rc_font *font_of(const rc_dc *d) { return rc_gdi_font_of(d); }

static const char *face_of(const rc_dc *d)
{
    gobj *f = rc_gobj_get(d->font ? d->font : rc_gdi_stock(13), OB_FONT);
    return f ? f->face : "System";
}

static void k_SetTextColor(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t old = d ? d->text_color : 0;
    if (d) {
        d->text_color = RC_ARG(c, 1) & 0xffffff;
    }
    RC_RETURN(c, old, 2);
}

static void k_SetTextAlign(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t old = d ? d->text_align : 0;
    if (d) {
        d->text_align = RC_ARG(c, 1);
    }
    RC_RETURN(c, old, 2);
}

/* The program's own fonts stay until the end. */
static void k_RemoveFontResourceA(rc_cpu *c) { RC_RETURN(c, 1, 1); }

/* ---- text ---- */

/* The code page of the current text: the font's character set (238 Central European, 204
 * Cyrillic), else the install's ANSI code page. */
static uint32_t text_codepage = 1252;

static uint32_t codepage_of_charset(uint32_t charset)
{
    switch (charset) {
    case 238: return 1250;
    case 204: return 1251;
    case 0:
    case 1: return rc_game_codepage();
    default: return 1252;
    }
}

/* ANSI text to UTF-8, in text_codepage. */
static void to_utf8(const uint8_t *s, uint32_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (uint32_t i = 0; i < n && o + 4 < cap; i++) {
        uint32_t cp = rc_codepage_unicode(text_codepage, s[i]);
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            out[o++] = (char)(0xc0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        } else {
            out[o++] = (char)(0xe0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    out[o] = 0;
}

/* The code page for text drawn with the DC's font. */
static void use_codepage(const rc_dc *d)
{
    gobj *f = d ? rc_gobj_get(d->font ? d->font : rc_gdi_stock(13), OB_FONT) : NULL;
    text_codepage = codepage_of_charset(f ? f->charset : 1);
}

static uint32_t to_unicode(uint8_t ch)
{
    char u[8];
    to_utf8(&ch, 1, u, sizeof u);
    const uint8_t *p = (const uint8_t *)u;
    if (p[0] < 0x80) {
        return p[0];
    }
    if (p[0] < 0xe0) {
        return ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
    }
    return ((uint32_t)(p[0] & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
}

/* ---- drawing text ---- */

typedef struct plotter {
    rc_dc *d;
    int32_t x, y;
    uint32_t v;
} plotter;

static void plot(void *ctx, int x, int y)
{
    plotter *p = ctx;
    rc_dc_plot(p->d, p->x + x, p->y + y, p->v);
}

int rc_gdi_text(rc_dc *d, int32_t x, int32_t y, const char *s, uint32_t n)
{
    char text[2048];
    use_codepage(d);
    to_utf8((const uint8_t *)s, n, text, sizeof text);
    rc_font *f = font_of(d);
    if (!f || !d->target.pixels) {
        return 0;
    }
    plotter p = {d, x, y, rc_dc_pixel(d, d->text_color)};
    return rc_font_draw(f, text, plot, &p);
}

void rc_gdi_text_size(rc_dc *d, const char *s, uint32_t n, int *w, int *h)
{
    char text[2048];
    use_codepage(d);
    to_utf8((const uint8_t *)s, n, text, sizeof text);
    rc_font *f = font_of(d);
    *w = 0;
    *h = 16;
    if (f) {
        rc_font_size(f, text, w, h);
    }
}

#define TA_RIGHT 2u
#define TA_CENTER 6u
#define TA_BOTTOM 8u
#define TA_BASELINE 24u
#define ETO_OPAQUE 2u
#define ETO_CLIPPED 4u

/* Text at (x, y) logical, aligned as the DC says; `opaque`, `clip`: ExtTextOut's rectangle. */
static void text_out(rc_dc *d, int32_t x, int32_t y, uint32_t s, uint32_t n, uint32_t options, uint32_t rect)
{
    char raw[2048];
    if (n > sizeof raw) {
        n = sizeof raw;
    }
    memcpy(raw, rc_ptr(s), n);
    int w = 0, h = 16;
    rc_gdi_text_size(d, raw, n, &w, &h);
    rc_font *f = font_of(d);
    rc_font_metrics m = {13, 3, 16, 0, 7, 14};
    if (f) {
        rc_font_metrics_of(f, &m);
    }
    x = rc_dc_x(d, x);
    y = rc_dc_y(d, y);
    uint32_t a = d->text_align;
    if ((a & TA_CENTER) == TA_CENTER) {
        x -= w / 2;
    } else if (a & TA_RIGHT) {
        x -= w;
    }
    if ((a & TA_BASELINE) == TA_BASELINE) {
        y -= m.ascent;
    } else if (a & TA_BOTTOM) {
        y -= h;
    }
    rc_rect r = {0, 0, 0, 0};
    if (rect) {
        r = (rc_rect){rc_dc_x(d, (int32_t)rc_ld32(rect)), rc_dc_y(d, (int32_t)rc_ld32(rect + 4)),
                      rc_dc_x(d, (int32_t)rc_ld32(rect + 8)), rc_dc_y(d, (int32_t)rc_ld32(rect + 12))};
    }
    if (rect && (options & ETO_OPAQUE)) {
        rc_dc_fill(d, r, d->bk_color);
    } else if (d->bk_mode != 1 && n) { /* OPAQUE */
        rc_dc_fill(d, (rc_rect){x, y, x + w, y + h}, d->bk_color);
    }
    bool had = d->has_clip;
    rc_rgn keep = d->clip;
    if (rect && (options & ETO_CLIPPED)) {
        if (!d->has_clip) {
            rc_rgn_set(&d->clip, -0x40000000, -0x40000000, 0x40000000, 0x40000000);
            d->has_clip = true;
        }
        rc_rgn_and_rect(&d->clip, r);
    }
    rc_gdi_text(d, x, y, raw, n);
    d->has_clip = had;
    d->clip = keep;
}

static void k_TextOutA(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (d) {
        text_out(d, (int32_t)RC_ARG(c, 1), (int32_t)RC_ARG(c, 2), RC_ARG(c, 3), RC_ARG(c, 4), 0, 0);
    }
    RC_RETURN(c, 1, 5);
}

/* ExtTextOutA(dc, x, y, options, rect, text, n, dx) */
static void k_ExtTextOutA(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (d) {
        text_out(d, (int32_t)RC_ARG(c, 1), (int32_t)RC_ARG(c, 2), RC_ARG(c, 5), RC_ARG(c, 6), RC_ARG(c, 3), RC_ARG(c, 4));
    }
    RC_RETURN(c, 1, 8);
}

/* GetTextExtentPoint32A / GetTextExtentPointA(dc, text, n, size) */
static void k_GetTextExtentPoint32A(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t n = RC_ARG(c, 2), out = RC_ARG(c, 3);
    if (!d) {
        RC_RETURN(c, 0, 4);
    }
    char raw[2048];
    if (n > sizeof raw) {
        n = sizeof raw;
    }
    memcpy(raw, rc_ptr(RC_ARG(c, 1)), n);
    int w, h;
    rc_gdi_text_size(d, raw, n, &w, &h);
    rc_font *f = font_of(d);
    if (f) {
        rc_font_metrics m;
        rc_font_metrics_of(f, &m);
        h = m.height;
    }
    rc_st32(out, (uint32_t)w);
    rc_st32(out + 4, (uint32_t)h);
    RC_RETURN(c, 1, 4);
}

/* GetTextMetricsA(dc, TEXTMETRICA) */
static void k_GetTextMetricsA(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t t = RC_ARG(c, 1);
    if (!d) {
        RC_RETURN(c, 0, 2);
    }
    rc_font *f = font_of(d);
    rc_font_metrics m = {13, 3, 16, 0, 7, 14};
    if (f) {
        rc_font_metrics_of(f, &m);
    }
    gobj *o = rc_gobj_get(d->font ? d->font : rc_gdi_stock(13), OB_FONT);
    memset(rc_ptr(t), 0, 56);
    rc_st32(t, (uint32_t)m.height);
    rc_st32(t + 4, (uint32_t)m.ascent);
    rc_st32(t + 8, (uint32_t)m.descent);
    rc_st32(t + 12, (uint32_t)m.internal_leading);
    rc_st32(t + 20, (uint32_t)m.avg_width);
    rc_st32(t + 24, (uint32_t)m.max_width);
    rc_st32(t + 28, o ? o->weight : 400);
    rc_st32(t + 36, 96);
    rc_st32(t + 40, 96);
    rc_st8(t + 44, 0x20);  /* first char */
    rc_st8(t + 45, 0xff);  /* last */
    rc_st8(t + 46, 0x1f);  /* default */
    rc_st8(t + 47, 0x20);  /* break */
    rc_st8(t + 48, (uint8_t)(o ? o->italic : 0));
    rc_st8(t + 49, (uint8_t)(o ? o->underline : 0));
    /* pitch and family: TMPF_FIXED_PITCH (1) means variable here, plus TrueType (4) */
    rc_st8(t + 51, (uint8_t)((o && o->pitch == 1) ? 0x04 : 0x05));
    RC_RETURN(c, 1, 2);
}

/* GetCharWidthA / GetCharWidth32A(dc, first, last, widths) */
static void k_GetCharWidthA(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t first = RC_ARG(c, 1), last = RC_ARG(c, 2), out = RC_ARG(c, 3);
    if (!d || last < first || last > 255) {
        RC_RETURN(c, 0, 4);
    }
    for (uint32_t ch = first; ch <= last; ch++) {
        char one = (char)ch;
        int w, h;
        rc_gdi_text_size(d, &one, 1, &w, &h);
        rc_st32(out + 4 * (ch - first), (uint32_t)w);
    }
    RC_RETURN(c, 1, 4);
}

static void k_GetCharABCWidthsA(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t first = RC_ARG(c, 1), last = RC_ARG(c, 2), out = RC_ARG(c, 3);
    rc_font *f = d ? font_of(d) : NULL;
    if (!f || last < first || last > 255) {
        RC_RETURN(c, 0, 4);
    }
    for (uint32_t ch = first; ch <= last; ch++) {
        int a = 0, b = 1, cc = 0;
        rc_font_abc(f, to_unicode((uint8_t)ch), &a, &b, &cc);
        rc_st32(out + 12 * (ch - first), (uint32_t)a);
        rc_st32(out + 12 * (ch - first) + 4, (uint32_t)b);
        rc_st32(out + 12 * (ch - first) + 8, (uint32_t)cc);
    }
    RC_RETURN(c, 1, 4);
}

static void k_GetTextFaceA(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t n = RC_ARG(c, 1), buf = RC_ARG(c, 2);
    const char *face = d ? face_of(d) : "System";
    uint32_t len = (uint32_t)strlen(face) + 1;
    if (!buf) {
        RC_RETURN(c, len, 3);
    }
    RC_RETURN(c, rc_put_str(buf, n, face) + 1, 3);
}

/* One ENUMLOGFONTEXA and NEWTEXTMETRICEXA per family the caller asks for. */
static uint32_t enum_face(rc_cpu *c, const char *face, uint32_t proc, uint32_t lparam)
{
    uint32_t elf = rc_heap_alloc(188, true), ntm = rc_heap_alloc(96, true);
    rc_st32(elf, (uint32_t)-16);
    rc_st32(elf + 16, 400);
    rc_st8(elf + 27, 0x22); /* VARIABLE_PITCH | FF_SWISS */
    rc_put_str(elf + 28, 32, face);
    rc_put_str(elf + 60, 64, face);
    rc_put_str(elf + 124, 32, "Regular");
    rc_put_str(elf + 156, 32, "Western");
    rc_font *f = rc_font_get(face, -16, 400, false);
    rc_font_metrics m = {13, 3, 16, 0, 7, 14};
    if (f) {
        rc_font_metrics_of(f, &m);
    }
    rc_st32(ntm, (uint32_t)m.height);
    rc_st32(ntm + 4, (uint32_t)m.ascent);
    rc_st32(ntm + 8, (uint32_t)m.descent);
    rc_st32(ntm + 12, (uint32_t)m.internal_leading);
    rc_st32(ntm + 20, (uint32_t)m.avg_width);
    rc_st32(ntm + 24, (uint32_t)m.max_width);
    rc_st32(ntm + 28, 400);
    rc_st32(ntm + 36, 96);
    rc_st32(ntm + 40, 96);
    rc_st8(ntm + 44, 0x20);
    rc_st8(ntm + 45, 0xff);
    rc_st8(ntm + 46, 0x1f);
    rc_st8(ntm + 47, 0x20);
    rc_st8(ntm + 51, 0x22);
    uint32_t args[4] = {elf, ntm, 4 /* TRUETYPE_FONTTYPE */, lparam};
    uint32_t r = rc_call_guest(c, proc, 4, args);
    rc_heap_free(elf);
    rc_heap_free(ntm);
    return r;
}

static void k_EnumFontFamiliesExA(rc_cpu *c)
{
    uint32_t lf = RC_ARG(c, 1), proc = RC_ARG(c, 2), lparam = RC_ARG(c, 3);
    const char *face = rc_str(lf + 28);
    uint32_t r = 1;
    if (face && *face) {
        r = enum_face(c, face, proc, lparam);
    } else {
        static const char *const all[] = {"Arial", "Courier New", "Times New Roman"};
        for (int i = 0; i < 3 && r; i++) {
            r = enum_face(c, all[i], proc, lparam);
        }
    }
    RC_RETURN(c, r, 5);
}

const rc_import_def rc_gdi32_text[] = {
    {G32, "CreateFontA", k_CreateFontA},
    {G32, "AddFontResourceA", k_AddFontResourceA},
    {G32, "CreateFontIndirectA", k_CreateFontIndirectA},
    {G32, "SetTextColor", k_SetTextColor},
    {G32, "SetTextAlign", k_SetTextAlign},
    {G32, "TextOutA", k_TextOutA},
    {G32, "ExtTextOutA", k_ExtTextOutA},
    {G32, "GetTextExtentPoint32A", k_GetTextExtentPoint32A},
    {G32, "GetTextExtentPointA", k_GetTextExtentPoint32A},
    {G32, "GetTextMetricsA", k_GetTextMetricsA},
    {G32, "GetCharWidthA", k_GetCharWidthA},
    {G32, "GetCharWidth32A", k_GetCharWidthA},
    {G32, "GetCharABCWidthsA", k_GetCharABCWidthsA},
    {G32, "GetTextFaceA", k_GetTextFaceA},
    {G32, "EnumFontFamiliesExA", k_EnumFontFamiliesExA},
    {G32, "RemoveFontResourceA", k_RemoveFontResourceA},
};
const uint32_t rc_gdi32_text_count = sizeof rc_gdi32_text / sizeof rc_gdi32_text[0];
