/* SPDX-License-Identifier: MIT */
/*
 * user32: drawing for windows and controls: the system colors (the classic Windows 2000 scheme)
 * and their brushes, DrawText, 3D edges, FillRect and friends, bitmaps from resources, and the
 * small string and DDE helpers programs built with MFC call.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdi32/dc.h"
#include "gdi32/gdi32.h"
#include "gdi32/objects.h"
#include "rc/hle.h"
#include "rc/log.h"
#include "rc/process.h"
#include "rc/text.h"
#include "rc/vm.h"
#include "user32.h"

#define U32 "USER32.dll"

/* ---- system colors ---- */

static const uint32_t sys_colors[31] = {
    0xc8d0d4, 0xa56e3a, 0x6a240a, 0x808080, 0xc8d0d4, 0xffffff, 0x000000, 0x000000, 0x000000, 0xffffff, 0xc8d0d4,
    0xc8d0d4, 0x808080, 0x6a240a, 0xffffff, 0xc8d0d4, 0x808080, 0x808080, 0x000000, 0xc8d0d4, 0xffffff, 0x404040,
    0xc8d0d4, 0x000000, 0xe1ffff, 0xb5b5b5, 0x800000, 0xf0caa6, 0xc0c0c0, 0xc8d0d4, 0xc8d0d4,
};

uint32_t rc_sys_color(uint32_t index) { return index < 31 ? sys_colors[index] : 0; }

uint32_t rc_sys_brush(uint32_t index)
{
    static uint32_t brushes[31];
    if (index >= 31) {
        return 0;
    }
    if (!brushes[index]) {
        brushes[index] = rc_gdi_solid_brush(sys_colors[index]);
        gobj *b = rc_gobj_get(brushes[index], OB_BRUSH);
        if (b) {
            b->stock = true;
        }
    }
    return brushes[index];
}

bool rc_brush_color(uint32_t brush, uint32_t *colorref)
{
    if (brush >= 1 && brush <= 31) {
        *colorref = sys_colors[brush - 1]; /* COLOR_* + 1 */
        return true;
    }
    gobj *b = rc_gobj_get(brush, OB_BRUSH);
    if (!b || b->style == 1 /* BS_NULL */) {
        return false;
    }
    *colorref = b->color;
    return true;
}

uint32_t rc_gui_font(void) { return rc_gdi_stock(17); }

static void k_GetSysColor(rc_cpu *c) { RC_RETURN(c, rc_sys_color(RC_ARG(c, 0)), 1); }
static void k_GetSysColorBrush(rc_cpu *c) { RC_RETURN(c, rc_sys_brush(RC_ARG(c, 0)), 1); }
static void k_SetSysColors(rc_cpu *c) { RC_RETURN(c, 1, 3); }

/* ---- rectangles ---- */

static rc_rect get_rect(uint32_t p)
{
    return (rc_rect){(int32_t)rc_ld32(p), (int32_t)rc_ld32(p + 4), (int32_t)rc_ld32(p + 8), (int32_t)rc_ld32(p + 12)};
}

static void put_rect(uint32_t p, rc_rect r)
{
    rc_st32(p, (uint32_t)r.l);
    rc_st32(p + 4, (uint32_t)r.t);
    rc_st32(p + 8, (uint32_t)r.r);
    rc_st32(p + 12, (uint32_t)r.b);
}

/* A rectangle in logical coordinates of a DC as device coordinates. */
static rc_rect dev_rect(const rc_dc *d, rc_rect r)
{
    return (rc_rect){rc_dc_x(d, r.l), rc_dc_y(d, r.t), rc_dc_x(d, r.r), rc_dc_y(d, r.b)};
}

static void fill_brush(rc_dc *d, rc_rect r, uint32_t brush)
{
    if (brush >= 1 && brush <= 31) {
        rc_dc_fill(d, r, sys_colors[brush - 1]);
    } else {
        rc_dc_pat(d, r, brush, 0x00f00021u); /* PATCOPY */
    }
}

/* FillRect(dc, rect, brush): the brush, or a COLOR_* + 1 value. */
static void k_FillRect(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (d) {
        fill_brush(d, dev_rect(d, get_rect(RC_ARG(c, 1))), RC_ARG(c, 2));
    }
    RC_RETURN(c, 1, 3);
}

/* FrameRect(dc, rect, brush): a line of the brush around the rectangle. */
static void k_FrameRect(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (d) {
        rc_rect r = dev_rect(d, get_rect(RC_ARG(c, 1)));
        uint32_t b = RC_ARG(c, 2);
        fill_brush(d, (rc_rect){r.l, r.t, r.r, r.t + 1}, b);
        fill_brush(d, (rc_rect){r.l, r.b - 1, r.r, r.b}, b);
        fill_brush(d, (rc_rect){r.l, r.t, r.l + 1, r.b}, b);
        fill_brush(d, (rc_rect){r.r - 1, r.t, r.r, r.b}, b);
    }
    RC_RETURN(c, 1, 3);
}

static void k_InvertRect(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (d) {
        rc_dc_pat(d, dev_rect(d, get_rect(RC_ARG(c, 1))), 0, 0x00550009u); /* DSTINVERT */
    }
    RC_RETURN(c, 1, 2);
}

/* DrawFocusRect(dc, rect): a dotted rectangle drawn by inverting, so twice removes it. */
static void focus_rect(rc_dc *d, rc_rect r)
{
    if (!d->target.pixels) {
        return;
    }
    for (int32_t x = r.l; x < r.r; x += 2) {
        rc_dc_pat(d, (rc_rect){x, r.t, x + 1, r.t + 1}, 0, 0x00550009u);
        rc_dc_pat(d, (rc_rect){x, r.b - 1, x + 1, r.b}, 0, 0x00550009u);
    }
    for (int32_t y = r.t + 2; y < r.b - 1; y += 2) {
        rc_dc_pat(d, (rc_rect){r.l, y, r.l + 1, y + 1}, 0, 0x00550009u);
        rc_dc_pat(d, (rc_rect){r.r - 1, y, r.r, y + 1}, 0, 0x00550009u);
    }
}

static void k_DrawFocusRect(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (d) {
        focus_rect(d, dev_rect(d, get_rect(RC_ARG(c, 1))));
    }
    RC_RETURN(c, 1, 2);
}

void rc_draw_focus(rc_dc *d, rc_rect r) { focus_rect(d, r); }

/* 3D edges: BDR_RAISEDOUTER 1, SUNKENOUTER 2, RAISEDINNER 4, SUNKENINNER 8; sides BF_LEFT 1,
 * TOP 2, RIGHT 4, BOTTOM 8, BF_MIDDLE 0x800, BF_ADJUST 0x2000, BF_FLAT 0x4000, BF_MONO 0x8000. */
static void edge_ring(rc_dc *d, rc_rect *r, uint32_t tl, uint32_t br, uint32_t flags)
{
    if (flags & 2) {
        rc_dc_fill(d, (rc_rect){r->l, r->t, r->r, r->t + 1}, tl);
    }
    if (flags & 1) {
        rc_dc_fill(d, (rc_rect){r->l, r->t, r->l + 1, r->b}, tl);
    }
    if (flags & 8) {
        rc_dc_fill(d, (rc_rect){r->l, r->b - 1, r->r, r->b}, br);
    }
    if (flags & 4) {
        rc_dc_fill(d, (rc_rect){r->r - 1, r->t, r->r, r->b}, br);
    }
    if (flags & 1) {
        r->l++;
    }
    if (flags & 2) {
        r->t++;
    }
    if (flags & 4) {
        r->r--;
    }
    if (flags & 8) {
        r->b--;
    }
}

void rc_draw_edge(rc_dc *d, rc_rect r, uint32_t edge, uint32_t flags)
{
    uint32_t light = rc_sys_color(22), hi = rc_sys_color(20), shadow = rc_sys_color(16), dk = rc_sys_color(21);
    if (flags & 0x8000) { /* BF_MONO */
        light = hi = shadow = dk = rc_sys_color(6);
    } else if (flags & 0x4000) { /* BF_FLAT */
        light = hi = shadow = dk = rc_sys_color(16);
    }
    if (edge & 3) { /* outer */
        bool raised = edge & 1;
        edge_ring(d, &r, raised ? light : shadow, raised ? dk : hi, flags);
    }
    if (edge & 0xc) { /* inner */
        bool raised = edge & 4;
        edge_ring(d, &r, raised ? hi : dk, raised ? shadow : light, flags);
    }
    if (flags & 0x800) {
        rc_dc_fill(d, r, rc_sys_color(15));
    }
}

/* DrawEdge(dc, rect, edge, flags) */
static void k_DrawEdge(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (d) {
        rc_rect r = dev_rect(d, get_rect(RC_ARG(c, 1)));
        rc_draw_edge(d, r, RC_ARG(c, 2), RC_ARG(c, 3));
        if (RC_ARG(c, 3) & 0x2000) { /* BF_ADJUST */
            int32_t n = ((RC_ARG(c, 2) & 3) ? 1 : 0) + ((RC_ARG(c, 2) & 0xc) ? 1 : 0);
            rc_rect a = get_rect(RC_ARG(c, 1));
            a.l += n, a.t += n, a.r -= n, a.b -= n;
            put_rect(RC_ARG(c, 1), a);
        }
    }
    RC_RETURN(c, 1, 4);
}

/* DrawFrameControl(dc, rect, type, state): buttons (DFC_BUTTON 4: check box, radio, push), scroll
 * arrows (DFC_SCROLL 3) and the menu check mark (DFC_MENU 2), drawn simply. */
static void k_DrawFrameControl(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t type = RC_ARG(c, 2), state = RC_ARG(c, 3);
    if (!d) {
        RC_RETURN(c, 0, 4);
    }
    rc_rect r = dev_rect(d, get_rect(RC_ARG(c, 1)));
    bool pushed = state & 0x200, checked = state & 0x400;
    if (type == 4 && (state & 0x1f) == 0x10) { /* DFCS_BUTTONPUSH */
        rc_dc_fill(d, r, rc_sys_color(15));
        rc_draw_edge(d, r, pushed ? 0xa : 0x5, 0xf);
    } else if (type == 4) {
        rc_dc_fill(d, r, rc_sys_color(5));
        rc_draw_edge(d, r, 0xa, 0xf);
        if (checked) {
            rc_dc_fill(d, (rc_rect){r.l + 3, r.t + 3, r.r - 3, r.b - 3}, rc_sys_color(8));
        }
    } else {
        rc_dc_fill(d, r, rc_sys_color(15));
        if (type == 3) {
            rc_draw_edge(d, r, pushed ? 0xa : 0x5, 0xf);
        } else if (type == 2 && checked) {
            rc_dc_fill(d, (rc_rect){r.l + 3, r.t + 3, r.r - 3, r.b - 3}, rc_sys_color(7));
        }
    }
    RC_RETURN(c, 1, 4);
}

/* ---- text ---- */

#define DT_CENTER 0x1u
#define DT_RIGHT 0x2u
#define DT_VCENTER 0x4u
#define DT_BOTTOM 0x8u
#define DT_WORDBREAK 0x10u
#define DT_SINGLELINE 0x20u
#define DT_EXPANDTABS 0x40u
#define DT_NOCLIP 0x100u
#define DT_CALCRECT 0x400u
#define DT_NOPREFIX 0x800u
#define DT_HIDEPREFIX 0x100000u
#define DT_END_ELLIPSIS 0x8000u
#define DT_PATH_ELLIPSIS 0x4000u
#define DT_WORD_ELLIPSIS 0x40000u

static int line_height(rc_dc *d)
{
    rc_font *f = rc_gdi_font_of(d);
    rc_font_metrics m = {13, 3, 16, 0, 7, 14};
    if (f) {
        rc_font_metrics_of(f, &m);
    }
    return m.height;
}

/* A line without its '&' marks (unless DT_NOPREFIX) into `out` (STRIP_MAX bytes); where the
 * underlined letter is (or -1). */
#define STRIP_MAX 1024
static int strip_prefix(const char *in, int n, char *out, uint32_t format)
{
    int k = 0, under = -1;
    for (int i = 0; i < n && k < STRIP_MAX - 1; i++) {
        char ch = in[i];
        if (ch == '\t' && (format & DT_EXPANDTABS)) {
            int stop = (k / 8 + 1) * 8;
            while (k < stop && k < STRIP_MAX - 1) {
                out[k++] = ' ';
            }
            continue;
        }
        if (ch == '&' && !(format & DT_NOPREFIX)) {
            if (i + 1 < n && in[i + 1] == '&') {
                out[k++] = '&';
                i++;
                continue;
            }
            if (i + 1 < n) {
                under = k;
            }
            continue;
        }
        out[k++] = ch;
    }
    out[k] = 0;
    return under;
}

static int width_of(rc_dc *d, const char *s, int n)
{
    int w = 0, h = 0;
    if (n > 0) {
        rc_gdi_text_size(d, s, (uint32_t)n, &w, &h);
    }
    return w;
}

int rc_draw_text(rc_dc *d, const char *text, int32_t n, rc_rect *r, uint32_t format)
{
    if (n < 0) {
        n = (int32_t)strlen(text);
    }
    int lh = line_height(d);
    /* lines: split at newlines, and at words where they do not fit (DT_WORDBREAK) */
    typedef struct {
        int start, len;
    } span;
    span lines[256];
    int nl = 0, maxw = 0;
    int width = r->r - r->l;
    for (int at = 0; at <= n && nl < 256;) {
        int end = at;
        while (end < n && (format & DT_SINGLELINE || (text[end] != '\n' && text[end] != '\r'))) {
            end++;
        }
        int len = end - at;
        if ((format & DT_WORDBREAK) && !(format & DT_SINGLELINE) && width > 0) {
            while (len > 0 && nl < 255) {
                char tmp[STRIP_MAX];
                int fit = len;
                strip_prefix(text + at, fit < 1000 ? fit : 1000, tmp, format);
                while (fit > 1 && width_of(d, tmp, (int)strlen(tmp)) > width) {
                    int sp = fit - 1;
                    while (sp > 0 && text[at + sp] != ' ') {
                        sp--;
                    }
                    fit = sp > 0 ? sp : fit - 1;
                    strip_prefix(text + at, fit, tmp, format);
                }
                if (fit >= len) {
                    break;
                }
                lines[nl++] = (span){at, fit};
                at += fit;
                while (at < end && text[at] == ' ') {
                    at++;
                }
                len = end - at;
            }
        }
        lines[nl++] = (span){at, end - at};
        if (end < n && text[end] == '\r' && end + 1 < n && text[end + 1] == '\n') {
            end++;
        }
        at = end + 1;
        if (end >= n) {
            break;
        }
    }
    for (int i = 0; i < nl; i++) {
        char tmp[STRIP_MAX];
        strip_prefix(text + lines[i].start, lines[i].len < 1000 ? lines[i].len : 1000, tmp, format);
        int w = width_of(d, tmp, (int)strlen(tmp));
        maxw = w > maxw ? w : maxw;
    }
    int total = nl * lh;
    if (format & DT_CALCRECT) {
        r->r = r->l + maxw;
        r->b = r->t + total;
        return total;
    }
    int32_t y = r->t;
    if (format & DT_SINGLELINE) {
        if (format & DT_VCENTER) {
            y = r->t + (r->b - r->t - lh) / 2;
        } else if (format & DT_BOTTOM) {
            y = r->b - lh;
        }
    }
    bool had = d->has_clip;
    rc_rgn keep = d->clip;
    if (!(format & DT_NOCLIP)) {
        if (!d->has_clip) {
            rc_rgn_set(&d->clip, -0x40000000, -0x40000000, 0x40000000, 0x40000000);
            d->has_clip = true;
        }
        rc_rgn_and_rect(&d->clip, *r);
    }
    for (int i = 0; i < nl; i++, y += lh) {
        char tmp[STRIP_MAX];
        int under = strip_prefix(text + lines[i].start, lines[i].len < 1000 ? lines[i].len : 1000, tmp, format);
        int len = (int)strlen(tmp), w = width_of(d, tmp, len);
        if ((format & (DT_END_ELLIPSIS | DT_PATH_ELLIPSIS | DT_WORD_ELLIPSIS)) && w > r->r - r->l && len > 0) {
            while (len > 0 && width_of(d, tmp, len) + width_of(d, "...", 3) > r->r - r->l) {
                len--;
            }
            memcpy(tmp + len, "...", 4);
            len += 3;
            w = width_of(d, tmp, len);
        }
        int32_t x = r->l;
        if (format & DT_CENTER) {
            x = r->l + (r->r - r->l - w) / 2;
        } else if (format & DT_RIGHT) {
            x = r->r - w;
        }
        if (d->bk_mode != 1) {
            rc_dc_fill(d, (rc_rect){x, y, x + w, y + lh}, d->bk_color);
        }
        rc_gdi_text(d, x, y, tmp, (uint32_t)len);
        if (under >= 0 && under < len && !(format & DT_HIDEPREFIX)) {
            int ux = width_of(d, tmp, under), uw = width_of(d, tmp + under, 1);
            rc_font *f = rc_gdi_font_of(d);
            rc_font_metrics m = {13, 3, 16, 0, 7, 14};
            if (f) {
                rc_font_metrics_of(f, &m);
            }
            rc_dc_fill(d, (rc_rect){x + ux, y + m.ascent + 1, x + ux + uw, y + m.ascent + 2}, d->text_color);
        }
    }
    d->has_clip = had;
    d->clip = keep;
    return total;
}

/* DrawTextA(dc, text, n, rect, format): logical coordinates. */
static void k_DrawTextA(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t s = RC_ARG(c, 1), rp = RC_ARG(c, 3), format = RC_ARG(c, 4);
    int32_t n = (int32_t)RC_ARG(c, 2);
    if (!d || !s) {
        RC_RETURN(c, 0, 5);
    }
    rc_rect lr = get_rect(rp), r = dev_rect(d, lr);
    const char *text = rc_str(s);
    if (n < 0) {
        n = (int32_t)strlen(text);
    }
    int h = rc_draw_text(d, text, n, &r, format);
    if (format & DT_CALCRECT) {
        lr.r = lr.l + (r.r - r.l);
        lr.b = lr.t + (r.b - r.t);
        put_rect(rp, lr);
    }
    RC_RETURN(c, (uint32_t)h, 5);
}

/* TabbedTextOutA(dc, x, y, text, n, tabs, positions, origin): tabs every 8 average characters. */
static void k_TabbedTextOutA(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, 0, 8);
    }
    rc_rect r = {rc_dc_x(d, (int32_t)RC_ARG(c, 1)), rc_dc_y(d, (int32_t)RC_ARG(c, 2)), 0x3fffffff, 0x3fffffff};
    int32_t n = (int32_t)RC_ARG(c, 4);
    const char *text = rc_str(RC_ARG(c, 3));
    rc_rect m = r;
    rc_draw_text(d, text, n, &m, DT_EXPANDTABS | DT_NOPREFIX | DT_SINGLELINE | DT_CALCRECT);
    rc_draw_text(d, text, n, &r, DT_EXPANDTABS | DT_NOPREFIX | DT_SINGLELINE | DT_NOCLIP);
    RC_RETURN(c, ((uint32_t)(m.b - m.t) << 16) | (uint32_t)(m.r - m.l), 8);
}

/* GetTabbedTextExtentA(dc, text, n, tabs, positions) */
static void k_GetTabbedTextExtentA(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, 0, 5);
    }
    rc_rect m = {0, 0, 0x3fffffff, 0x3fffffff};
    rc_draw_text(d, rc_str(RC_ARG(c, 1)), (int32_t)RC_ARG(c, 2), &m, DT_EXPANDTABS | DT_NOPREFIX | DT_SINGLELINE | DT_CALCRECT);
    RC_RETURN(c, ((uint32_t)(m.b - m.t) << 16) | (uint32_t)(m.r - m.l), 5);
}

/* GrayStringA(dc, brush, proc, data, n, x, y, w, h): with a procedure, it draws (normally);
 * else the text in the gray text color. */
static void k_GrayStringA(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    uint32_t proc = RC_ARG(c, 2), data = RC_ARG(c, 3);
    int32_t n = (int32_t)RC_ARG(c, 4);
    if (!d) {
        RC_RETURN(c, 0, 9);
    }
    if (proc) {
        uint32_t args[3] = {RC_ARG(c, 0), data, (uint32_t)n};
        RC_RETURN(c, rc_call_guest(c, proc, 3, args), 9);
    }
    uint32_t keep = d->text_color;
    d->text_color = rc_sys_color(17);
    rc_rect r = {rc_dc_x(d, (int32_t)RC_ARG(c, 5)), rc_dc_y(d, (int32_t)RC_ARG(c, 6)), 0x3fffffff, 0x3fffffff};
    rc_draw_text(d, rc_str(data), n ? n : -1, &r, DT_NOCLIP | DT_SINGLELINE);
    d->text_color = keep;
    RC_RETURN(c, 1, 9);
}

/* Icons are handles without pictures here: drawing one draws nothing. */
static void k_DrawIcon(rc_cpu *c) { RC_RETURN(c, 1, 4); }
static void k_DrawIconEx(rc_cpu *c) { RC_RETURN(c, 1, 9); }

/* ---- bitmaps from resources ---- */

uint32_t rc_load_bitmap(uint32_t inst, uint32_t name)
{
    uint32_t data, size;
    bool found = name < 0x10000 ? rc_resource(inst, 2, name, &data, &size)
                                : rc_resource_named(inst, 2, rc_str(name), &data, &size);
    if (!inst || !found) {
        /* a system bitmap (OBM_*), or one that is not there: a small gray one */
        uint32_t bmi = rc_heap_alloc(40 + 16 * 16 * 4, true);
        rc_st32(bmi, 40);
        rc_st32(bmi + 4, 16);
        rc_st32(bmi + 8, 16);
        rc_st16(bmi + 12, 1);
        rc_st16(bmi + 14, 32);
        for (uint32_t i = 0; i < 256; i++) {
            rc_st32(bmi + 40 + 4 * i, 0xc0c0c0);
        }
        uint32_t h = rc_gdi_bitmap_from_dib(bmi, bmi + 40);
        rc_heap_free(bmi);
        return h;
    }
    uint32_t hsize = rc_ld32(data), bpp = rc_ld16(data + 14), used = rc_ld32(data + 32);
    uint32_t colors = bpp <= 8 ? (used ? used : 1u << bpp) : (rc_ld32(data + 16) == 3 ? 3 : 0);
    return rc_gdi_bitmap_from_dib(data, data + hsize + 4 * colors);
}

static void k_LoadBitmapA(rc_cpu *c) { RC_RETURN(c, rc_load_bitmap(RC_ARG(c, 0), RC_ARG(c, 1)), 2); }

/* LoadImageA(instance, name, type, cx, cy, flags): bitmaps here; icons and cursors from user32.c. */

static void k_LoadImageA(rc_cpu *c)
{
    uint32_t type = RC_ARG(c, 2);
    if (type == 0) {
        RC_RETURN(c, rc_load_bitmap(RC_ARG(c, 0), RC_ARG(c, 1)), 6);
    }
    RC_RETURN(c, rc_load_icon_or_cursor(c, RC_ARG(c, 0), RC_ARG(c, 1), type), 6);
}

/* CopyImage(image, type, cx, cy, flags): the same image. */
static void k_CopyImage(rc_cpu *c) { RC_RETURN(c, RC_ARG(c, 0), 5); }

/* ---- characters ---- */

static void k_CharUpperA(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    if (p < 0x10000) {
        RC_RETURN(c, (uint32_t)toupper((int)(p & 0xff)), 1);
    }
    for (uint32_t a = p; rc_ld8(a); a++) {
        rc_st8(a, (uint8_t)toupper(rc_ld8(a)));
    }
    RC_RETURN(c, p, 1);
}

static void k_CharLowerA(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    if (p < 0x10000) {
        RC_RETURN(c, (uint32_t)tolower((int)(p & 0xff)), 1);
    }
    for (uint32_t a = p; rc_ld8(a); a++) {
        rc_st8(a, (uint8_t)tolower(rc_ld8(a)));
    }
    RC_RETURN(c, p, 1);
}

static void k_CharUpperBuffA(rc_cpu *c)
{
    for (uint32_t i = 0; i < RC_ARG(c, 1); i++) {
        rc_st8(RC_ARG(c, 0) + i, (uint8_t)toupper(rc_ld8(RC_ARG(c, 0) + i)));
    }
    RC_RETURN(c, RC_ARG(c, 1), 2);
}

static void k_CharLowerBuffA(rc_cpu *c)
{
    for (uint32_t i = 0; i < RC_ARG(c, 1); i++) {
        rc_st8(RC_ARG(c, 0) + i, (uint8_t)tolower(rc_ld8(RC_ARG(c, 0) + i)));
    }
    RC_RETURN(c, RC_ARG(c, 1), 2);
}

static void k_CharNextA(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    RC_RETURN(c, rc_ld8(p) ? p + 1 : p, 1);
}

static void k_CharPrevA(rc_cpu *c)
{
    uint32_t start = RC_ARG(c, 0), p = RC_ARG(c, 1);
    RC_RETURN(c, p > start ? p - 1 : start, 2);
}

static void k_IsCharAlphaA(rc_cpu *c) { RC_RETURN(c, isalpha((int)(RC_ARG(c, 0) & 0xff)) != 0 || (RC_ARG(c, 0) & 0xff) >= 0xc0, 1); }
static void k_IsCharAlphaNumericA(rc_cpu *c)
{
    int ch = (int)(RC_ARG(c, 0) & 0xff);
    RC_RETURN(c, isalnum(ch) != 0 || ch >= 0xc0, 1);
}
static void k_IsCharUpperA(rc_cpu *c) { RC_RETURN(c, isupper((int)(RC_ARG(c, 0) & 0xff)) != 0, 1); }
static void k_IsCharLowerA(rc_cpu *c) { RC_RETURN(c, islower((int)(RC_ARG(c, 0) & 0xff)) != 0, 1); }

/* CharToOemA / OemToCharA(src, dst): a copy. */
static void k_CharToOemA(rc_cpu *c)
{
    const char *s = rc_str(RC_ARG(c, 0));
    memmove(rc_ptr(RC_ARG(c, 1)), s, strlen(s) + 1);
    RC_RETURN(c, 1, 2);
}

/* ---- odds and ends ---- */

static void k_MessageBeep(rc_cpu *c) { RC_RETURN(c, 1, 1); }
static void k_FlashWindow(rc_cpu *c) { RC_RETURN(c, 0, 2); }
static void k_WinHelpA(rc_cpu *c)
{
    rc_log("WinHelpA: no help file is shown");
    RC_RETURN(c, 0, 4);
}

/* DDE parameters: the two words of lParam (no packing needed in one address space). */
static void k_UnpackDDElParam(rc_cpu *c)
{
    uint32_t lp = RC_ARG(c, 1);
    if (RC_ARG(c, 2)) {
        rc_st32(RC_ARG(c, 2), lp & 0xffff);
    }
    if (RC_ARG(c, 3)) {
        rc_st32(RC_ARG(c, 3), lp >> 16);
    }
    RC_RETURN(c, 1, 4);
}
static void k_ReuseDDElParam(rc_cpu *c) { RC_RETURN(c, (RC_ARG(c, 3) & 0xffff) | (RC_ARG(c, 4) << 16), 5); }
static void k_PackDDElParam(rc_cpu *c) { RC_RETURN(c, (RC_ARG(c, 1) & 0xffff) | (RC_ARG(c, 2) << 16), 3); }
static void k_FreeDDElParam(rc_cpu *c) { RC_RETURN(c, 1, 2); }

const rc_import_def rc_user32_draw[] = {
    {U32, "GetSysColor", k_GetSysColor},
    {U32, "GetSysColorBrush", k_GetSysColorBrush},
    {U32, "SetSysColors", k_SetSysColors},
    {U32, "FillRect", k_FillRect},
    {U32, "FrameRect", k_FrameRect},
    {U32, "InvertRect", k_InvertRect},
    {U32, "DrawFocusRect", k_DrawFocusRect},
    {U32, "DrawEdge", k_DrawEdge},
    {U32, "DrawFrameControl", k_DrawFrameControl},
    {U32, "DrawTextA", k_DrawTextA},
    {U32, "TabbedTextOutA", k_TabbedTextOutA},
    {U32, "GetTabbedTextExtentA", k_GetTabbedTextExtentA},
    {U32, "GrayStringA", k_GrayStringA},
    {U32, "DrawIcon", k_DrawIcon},
    {U32, "DrawIconEx", k_DrawIconEx},
    {U32, "LoadBitmapA", k_LoadBitmapA},
    {U32, "LoadImageA", k_LoadImageA},
    {U32, "CopyImage", k_CopyImage},
    {U32, "CharUpperA", k_CharUpperA},
    {U32, "CharLowerA", k_CharLowerA},
    {U32, "CharUpperBuffA", k_CharUpperBuffA},
    {U32, "CharLowerBuffA", k_CharLowerBuffA},
    {U32, "CharNextA", k_CharNextA},
    {U32, "CharPrevA", k_CharPrevA},
    {U32, "IsCharAlphaA", k_IsCharAlphaA},
    {U32, "IsCharAlphaNumericA", k_IsCharAlphaNumericA},
    {U32, "IsCharUpperA", k_IsCharUpperA},
    {U32, "IsCharLowerA", k_IsCharLowerA},
    {U32, "CharToOemA", k_CharToOemA},
    {U32, "OemToCharA", k_CharToOemA},
    {U32, "MessageBeep", k_MessageBeep},
    {U32, "FlashWindow", k_FlashWindow},
    {U32, "WinHelpA", k_WinHelpA},
    {U32, "UnpackDDElParam", k_UnpackDDElParam},
    {U32, "ReuseDDElParam", k_ReuseDDElParam},
    {U32, "PackDDElParam", k_PackDDElParam},
    {U32, "FreeDDElParam", k_FreeDDElParam},
};
const uint32_t rc_user32_draw_count = sizeof rc_user32_draw / sizeof rc_user32_draw[0];
