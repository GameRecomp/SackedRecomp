/* SPDX-License-Identifier: MIT */
/*
 * COMCTL32: the common controls programs built with MFC use: the toolbar, the status bar, the
 * progress bar, the trackbar, and tool tips (which take their messages and show nothing). They
 * are window procedures of our own on user32's windows, drawn with the drawing core.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "comctl32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdi32/dc.h"
#include "gdi32/objects.h"
#include "rc/log.h"
#include "rc/platform.h"
#include "rc/text.h"
#include "rc/vm.h"
#include "user32/user32.h"

#define CC "COMCTL32.dll"

#define WM_DESTROY 0x0002u
#define WM_SIZE 0x0005u
#define WM_SETTEXT 0x000cu
#define WM_GETTEXT 0x000du
#define WM_GETTEXTLENGTH 0x000eu
#define WM_PAINT 0x000fu
#define WM_ERASEBKGND 0x0014u
#define WM_GETFONT 0x0031u
#define WM_NCHITTEST 0x0084u
#define WM_GETDLGCODE 0x0087u
#define WM_KEYDOWN 0x0100u
#define WM_COMMAND 0x0111u
#define WM_HSCROLL 0x0114u
#define WM_VSCROLL 0x0115u
#define WM_MOUSEMOVE 0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP 0x0202u
#define WM_LBUTTONDBLCLK 0x0203u
#define WM_SETFOCUS 0x0007u
#define WM_KILLFOCUS 0x0008u
#define WM_ENABLE 0x000au

#define RET(v)                                                                                                         \
    do {                                                                                                               \
        RC_RETURN(c, (v), 4);                                                                                          \
    } while (0)

/* Paint a control: a DC with its font; the caller releases it. */
static rc_dc *paint_dc(rc_cpu *c, uint32_t hwnd, uint32_t *hdc)
{
    rc_wnd_validate(hwnd);
    *hdc = rc_wnd_dc(hwnd, false);
    rc_dc *d = rc_dc_get(*hdc);
    if (d) {
        uint32_t font = rc_def_window_proc(c, hwnd, WM_GETFONT, 0, 0);
        d->font = font ? font : rc_gui_font();
        d->bk_mode = 1;
        d->text_color = rc_sys_color(18);
    }
    return d;
}

static void *data_of(uint32_t hwnd, size_t size)
{
    void *p = rc_wnd_data(hwnd);
    if (!p) {
        p = calloc(1, size);
        rc_wnd_set_data(hwnd, p);
    }
    return p;
}

static int32_t font_h(rc_cpu *c, uint32_t hwnd)
{
    uint32_t hdc;
    rc_dc *d = paint_dc(c, hwnd, &hdc);
    int32_t h = 13;
    if (d) {
        rc_font *f = rc_gdi_font_of(d);
        rc_font_metrics m;
        if (f) {
            rc_font_metrics_of(f, &m);
            h = m.height;
        }
    }
    rc_wnd_release_dc(hdc);
    return h;
}

/* Common controls along an edge of their parent (CCS_TOP 1, CCS_BOTTOM 3) size themselves. */
static void dock(rc_cpu *c, uint32_t hwnd, int32_t height)
{
    uint32_t style = rc_wnd_style(hwnd), parent = rc_wnd_parent(hwnd);
    if ((style & 0x8 /* CCS_NOPARENTALIGN */) || (style & 0x4 /* CCS_NORESIZE */) || !parent) {
        return;
    }
    rc_rect pr = rc_wnd_client(parent);
    bool bottom = (style & 3) == 3;
    rc_wnd_move(c, hwnd, 0, bottom ? pr.b - height : 0, pr.r, height, true);
}

/* ---- status bar ---- */

typedef struct statusbar {
    int32_t parts[32];
    char *text[32];
    int n;
    bool simple;
    char *simple_text;
} statusbar;

static void sb_paint(rc_cpu *c, uint32_t hwnd, statusbar *s)
{
    uint32_t hdc;
    rc_dc *d = paint_dc(c, hwnd, &hdc);
    if (!d) {
        return;
    }
    rc_rect r = rc_wnd_client(hwnd);
    rc_dc_fill(d, r, rc_sys_color(15));
    int32_t x = 0;
    int n = s->simple ? 1 : (s->n ? s->n : 1);
    for (int i = 0; i < n; i++) {
        int32_t right = s->simple || !s->n ? r.r : s->parts[i] < 0 ? r.r : s->parts[i];
        if (right > r.r) {
            right = r.r;
        }
        rc_rect pr = {x, r.t + 2, right - 2, r.b};
        rc_draw_edge(d, pr, 0x2 /* SUNKENOUTER */, 0xf);
        const char *t = s->simple ? s->simple_text : s->text[i];
        if (t) {
            rc_rect tr = {pr.l + 3, pr.t, pr.r - 2, pr.b};
            rc_draw_text(d, t, -1, &tr, 0x8824 /* VCENTER SINGLELINE NOPREFIX END_ELLIPSIS */);
        }
        x = right;
    }
    if (rc_wnd_style(hwnd) & 0x100) { /* SBARS_SIZEGRIP */
        for (int k = 0; k < 3; k++) {
            int32_t o = 4 * k + 3;
            rc_dc_fill(d, (rc_rect){r.r - o, r.b - 2, r.r - o + 1, r.b - 1}, rc_sys_color(16));
        }
    }
    rc_wnd_release_dc(hdc);
}

static void wp_statusbar(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), msg = RC_ARG(c, 1), wp = RC_ARG(c, 2), lp = RC_ARG(c, 3);
    statusbar *s = data_of(hwnd, sizeof *s);
    switch (msg) {
    case 0x0001: /* WM_CREATE */
    case WM_SIZE:
        if (msg == 0x0001 || (rc_wnd_style(hwnd) & 3)) {
            dock(c, hwnd, font_h(c, hwnd) + 8);
        }
        RET(0);
    case WM_PAINT:
        sb_paint(c, hwnd, s);
        RET(0);
    case WM_ERASEBKGND:
        RET(1);
    case 0x404: /* SB_SETPARTS(n, widths) */
        s->n = wp < 32 ? (int)wp : 32;
        for (int i = 0; i < s->n; i++) {
            s->parts[i] = (int32_t)rc_ld32(lp + 4 * (uint32_t)i);
        }
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(1);
    case 0x406: /* SB_GETPARTS(n, widths) */
        for (int i = 0; i < s->n && i < (int)wp; i++) {
            rc_st32(lp + 4 * (uint32_t)i, (uint32_t)s->parts[i]);
        }
        RET((uint32_t)s->n);
    case 0x401: { /* SB_SETTEXTA(part | type, text) */
        uint32_t part = wp & 0xff;
        char *t = strdup(lp ? rc_str(lp) : "");
        if (part == 0xff) {
            free(s->simple_text);
            s->simple_text = t;
        } else if (part < 32) {
            free(s->text[part]);
            s->text[part] = t;
        } else {
            free(t);
        }
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(1);
    }
    case 0x402: { /* SB_GETTEXTA */
        const char *t = (wp & 0xff) < 32 && s->text[wp & 0xff] ? s->text[wp & 0xff] : "";
        if (lp) {
            rc_put_str(lp, 0x7fffffff, t);
        }
        RET((uint32_t)strlen(t));
    }
    case 0x403: { /* SB_GETTEXTLENGTHA */
        const char *t = (wp & 0xff) < 32 && s->text[wp & 0xff] ? s->text[wp & 0xff] : "";
        RET((uint32_t)strlen(t));
    }
    case 0x407: /* SB_GETBORDERS */
        rc_st32(lp, 0);
        rc_st32(lp + 4, 2);
        rc_st32(lp + 8, 2);
        RET(1);
    case 0x408: /* SB_SETMINHEIGHT */
        RET(0);
    case 0x409: /* SB_SIMPLE */
        s->simple = wp != 0;
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(0);
    case 0x40a: { /* SB_GETRECT(part, rect) */
        rc_rect r = rc_wnd_client(hwnd);
        int32_t l = 0;
        for (uint32_t i = 0; i < wp && (int)i < s->n; i++) {
            l = s->parts[i];
        }
        int32_t right = (int)wp < s->n && s->parts[wp] >= 0 ? s->parts[wp] : r.r;
        rc_st32(lp, (uint32_t)l);
        rc_st32(lp + 4, 2);
        rc_st32(lp + 8, (uint32_t)right);
        rc_st32(lp + 12, (uint32_t)r.b);
        RET(1);
    }
    case WM_SETTEXT:
        free(s->text[0]);
        s->text[0] = strdup(lp ? rc_str(lp) : "");
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(1);
    case WM_GETTEXT:
        RET(wp ? rc_put_str(lp, wp, s->text[0] ? s->text[0] : "") : 0);
    case WM_GETTEXTLENGTH:
        RET(s->text[0] ? (uint32_t)strlen(s->text[0]) : 0);
    case WM_DESTROY:
        for (int i = 0; i < 32; i++) {
            free(s->text[i]);
            s->text[i] = NULL;
        }
        free(s->simple_text);
        s->simple_text = NULL;
        RET(0);
    default:
        RET(rc_def_window_proc(c, hwnd, msg, wp, lp));
    }
}

/* ---- progress bar ---- */

typedef struct progress {
    int32_t lo, hi, pos, step;
} progress;

static void wp_progress(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), msg = RC_ARG(c, 1), wp = RC_ARG(c, 2), lp = RC_ARG(c, 3);
    progress *p = data_of(hwnd, sizeof *p);
    if (!p->hi && !p->lo && !p->step) {
        p->hi = 100;
        p->step = 10;
    }
    int32_t old = p->pos;
    switch (msg) {
    case WM_PAINT: {
        uint32_t hdc;
        rc_dc *d = paint_dc(c, hwnd, &hdc);
        if (d) {
            rc_rect r = rc_wnd_client(hwnd);
            rc_dc_fill(d, r, rc_sys_color(15));
            rc_draw_edge(d, r, 0x2, 0xf);
            int32_t range = p->hi - p->lo;
            int32_t w = range > 0 ? (int32_t)((int64_t)(r.r - 2) * (p->pos - p->lo) / range) : 0;
            rc_dc_fill(d, (rc_rect){1, 1, 1 + (w < 0 ? 0 : w), r.b - 1}, rc_sys_color(13));
        }
        rc_wnd_release_dc(hdc);
        RET(0);
    }
    case WM_ERASEBKGND: RET(1);
    case 0x401: p->lo = (int32_t)(lp & 0xffff); p->hi = (int32_t)(lp >> 16); RET(0); /* PBM_SETRANGE */
    case 0x406: p->lo = (int32_t)wp; p->hi = (int32_t)lp; RET(0);                   /* PBM_SETRANGE32 */
    case 0x402: p->pos = (int32_t)wp; break;                                          /* PBM_SETPOS */
    case 0x403: p->pos += (int32_t)wp; break;                                         /* PBM_DELTAPOS */
    case 0x404: { int32_t s = p->step; p->step = (int32_t)wp; RET((uint32_t)s); }     /* PBM_SETSTEP */
    case 0x405: p->pos += p->step; if (p->pos > p->hi) { p->pos = p->lo + (p->pos - p->hi); } break; /* PBM_STEPIT */
    case 0x407: RET((uint32_t)(wp ? p->lo : p->hi));                                 /* PBM_GETRANGE */
    case 0x408: RET((uint32_t)p->pos);                                               /* PBM_GETPOS */
    default: RET(rc_def_window_proc(c, hwnd, msg, wp, lp));
    }
    p->pos = p->pos < p->lo ? p->lo : p->pos > p->hi ? p->hi : p->pos;
    rc_wnd_invalidate(hwnd, NULL, false);
    RET((uint32_t)old);
}

/* ---- trackbar ---- */

typedef struct trackbar {
    int32_t lo, hi, pos, page, line;
    bool tracking;
} trackbar;

static bool tb_vertical(uint32_t hwnd) { return rc_wnd_style(hwnd) & 2; }

/* The channel and the thumb (client coordinates). */
static void tb_geometry(uint32_t hwnd, trackbar *t, rc_rect *channel, rc_rect *thumb)
{
    rc_rect r = rc_wnd_client(hwnd);
    int32_t range = t->hi - t->lo;
    if (tb_vertical(hwnd)) {
        int32_t cx = r.r / 2;
        *channel = (rc_rect){cx - 2, 8, cx + 2, r.b - 8};
        int32_t y = channel->t + (range > 0 ? (int32_t)((int64_t)(channel->b - channel->t) * (t->pos - t->lo) / range) : 0);
        *thumb = (rc_rect){cx - 10, y - 5, cx + 10, y + 6};
    } else {
        int32_t cy = r.b / 2;
        *channel = (rc_rect){8, cy - 2, r.r - 8, cy + 2};
        int32_t x = channel->l + (range > 0 ? (int32_t)((int64_t)(channel->r - channel->l) * (t->pos - t->lo) / range) : 0);
        *thumb = (rc_rect){x - 5, cy - 10, x + 6, cy + 10};
    }
}

static void tb_set(rc_cpu *c, uint32_t hwnd, trackbar *t, int32_t pos, uint32_t code)
{
    pos = pos < t->lo ? t->lo : pos > t->hi ? t->hi : pos;
    if (pos != t->pos || code == 8) {
        t->pos = pos;
        rc_wnd_invalidate(hwnd, NULL, true);
        uint32_t parent = rc_wnd_parent(hwnd);
        if (parent) {
            rc_wnd_send(c, parent, tb_vertical(hwnd) ? WM_VSCROLL : WM_HSCROLL, code | ((uint32_t)(pos & 0xffff) << 16), hwnd);
        }
    }
}

static int32_t tb_pos_at(uint32_t hwnd, trackbar *t, int32_t x, int32_t y)
{
    rc_rect ch, th;
    tb_geometry(hwnd, t, &ch, &th);
    int32_t len = tb_vertical(hwnd) ? ch.b - ch.t : ch.r - ch.l, at = tb_vertical(hwnd) ? y - ch.t : x - ch.l;
    return len > 0 ? t->lo + (int32_t)(((int64_t)at * (t->hi - t->lo) + len / 2) / len) : t->lo;
}

static void wp_trackbar(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), msg = RC_ARG(c, 1), wp = RC_ARG(c, 2), lp = RC_ARG(c, 3);
    trackbar *t = data_of(hwnd, sizeof *t);
    if (!t->hi && !t->lo && !t->page) {
        t->hi = 100;
        t->page = 20;
        t->line = 1;
    }
    switch (msg) {
    case WM_PAINT: {
        uint32_t hdc;
        rc_dc *d = paint_dc(c, hwnd, &hdc);
        if (d) {
            rc_rect r = rc_wnd_client(hwnd), ch, th;
            rc_dc_fill(d, r, rc_sys_color(15));
            tb_geometry(hwnd, t, &ch, &th);
            rc_draw_edge(d, ch, 0xa, 0xf);
            rc_dc_fill(d, th, rc_sys_color(15));
            rc_draw_edge(d, th, 0x5, 0xf);
            if (rc_wnd_focus() == hwnd) {
                rc_draw_focus(d, r);
            }
        }
        rc_wnd_release_dc(hdc);
        RET(0);
    }
    case WM_ERASEBKGND: RET(1);
    case WM_GETDLGCODE: RET(1 /* DLGC_WANTARROWS */);
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(0);
    case WM_LBUTTONDOWN: {
        rc_wnd_set_focus(c, hwnd);
        int32_t x = (int16_t)(lp & 0xffff), y = (int16_t)(lp >> 16);
        rc_rect ch, th;
        tb_geometry(hwnd, t, &ch, &th);
        if (x >= th.l && x < th.r && y >= th.t && y < th.b) {
            t->tracking = true;
            rc_wnd_set_capture(hwnd);
        } else {
            int32_t at = tb_pos_at(hwnd, t, x, y);
            tb_set(c, hwnd, t, at < t->pos ? t->pos - t->page : t->pos + t->page, at < t->pos ? 2 : 3);
        }
        RET(0);
    }
    case WM_MOUSEMOVE:
        if (t->tracking) {
            tb_set(c, hwnd, t, tb_pos_at(hwnd, t, (int16_t)(lp & 0xffff), (int16_t)(lp >> 16)), 5 /* TB_THUMBTRACK */);
        }
        RET(0);
    case WM_LBUTTONUP:
        if (t->tracking) {
            t->tracking = false;
            rc_wnd_set_capture(0);
            tb_set(c, hwnd, t, t->pos, 4 /* TB_THUMBPOSITION */);
        }
        tb_set(c, hwnd, t, t->pos, 8 /* TB_ENDTRACK */);
        RET(0);
    case WM_KEYDOWN: {
        int32_t p = t->pos;
        switch (wp) {
        case 0x25: case 0x26: p -= t->line; break;
        case 0x27: case 0x28: p += t->line; break;
        case 0x21: p -= t->page; break;
        case 0x22: p += t->page; break;
        case 0x24: p = t->lo; break;
        case 0x23: p = t->hi; break;
        default: RET(0);
        }
        tb_set(c, hwnd, t, p, wp == 0x21 ? 2 : wp == 0x22 ? 3 : (wp == 0x25 || wp == 0x26) ? 0 : 1);
        tb_set(c, hwnd, t, t->pos, 8);
        RET(0);
    }
    case 0x400: RET((uint32_t)t->pos);                                         /* TBM_GETPOS */
    case 0x401: RET((uint32_t)t->lo);                                          /* TBM_GETRANGEMIN */
    case 0x402: RET((uint32_t)t->hi);                                          /* TBM_GETRANGEMAX */
    case 0x405: t->pos = (int32_t)lp < t->lo ? t->lo : (int32_t)lp > t->hi ? t->hi : (int32_t)lp; break; /* TBM_SETPOS */
    case 0x406: t->lo = (int16_t)(lp & 0xffff); t->hi = (int16_t)(lp >> 16); break;                   /* TBM_SETRANGE */
    case 0x407: t->lo = (int32_t)lp; break;                                                          /* TBM_SETRANGEMIN */
    case 0x408: t->hi = (int32_t)lp; break;                                                          /* TBM_SETRANGEMAX */
    case 0x415: { int32_t o = t->page; t->page = (int32_t)lp; RET((uint32_t)o); }                    /* TBM_SETPAGESIZE */
    case 0x416: RET((uint32_t)t->page);
    case 0x417: { int32_t o = t->line; t->line = (int32_t)lp; RET((uint32_t)o); }                    /* TBM_SETLINESIZE */
    case 0x418: RET((uint32_t)t->line);
    case 0x403: case 0x404: case 0x409: case 0x40a: case 0x40b: case 0x40c: case 0x414: RET(0);    /* tics, selection */
    case 0x410: RET(0);                                                                             /* TBM_GETNUMTICS */
    default: RET(rc_def_window_proc(c, hwnd, msg, wp, lp));
    }
    if (t->pos < t->lo) {
        t->pos = t->lo;
    }
    if (t->pos > t->hi) {
        t->pos = t->hi;
    }
    rc_wnd_invalidate(hwnd, NULL, true);
    RET(0);
}

/* ---- tool tips: messages taken, nothing shown ---- */

static void wp_tooltips(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), msg = RC_ARG(c, 1), wp = RC_ARG(c, 2), lp = RC_ARG(c, 3);
    if (msg == WM_NCHITTEST) {
        RET(0xffffffffu);
    }
    if (msg >= 0x400 && msg < 0x500) {
        RET(msg == 0x404 /* TTM_ADDTOOLA */ ? 1 : 0);
    }
    RET(rc_def_window_proc(c, hwnd, msg, wp, lp));
}

/* ---- toolbar ---- */

typedef struct tbutton {
    int32_t image;
    uint32_t id;
    uint8_t state, style;
    uint32_t data;
    int32_t string;
} tbutton;

typedef struct toolbar {
    tbutton *b;
    int n, cap;
    uint32_t bitmaps[16];
    int32_t bitmap_base[16];
    int nbitmaps, images;
    int32_t bw, bh, iw, ih, indent;
    uint32_t owner;
    int pressed;
    bool tracking;
} toolbar;

static toolbar *tbd(uint32_t hwnd)
{
    toolbar *t = data_of(hwnd, sizeof *t);
    if (!t->bw) {
        t->bw = 24;
        t->bh = 22;
        t->iw = 16;
        t->ih = 15;
        t->pressed = -1;
    }
    return t;
}

static rc_rect tbtn_rect(toolbar *t, int i)
{
    int32_t x = t->indent;
    for (int k = 0; k < i; k++) {
        if (t->b[k].state & 0x8) { /* TBSTATE_HIDDEN */
            continue;
        }
        x += (t->b[k].style & 1) ? (t->b[k].image > 0 ? t->b[k].image : 8) : t->bw;
    }
    int32_t w = (t->b[i].style & 1) ? (t->b[i].image > 0 ? t->b[i].image : 8) : t->bw;
    return (rc_rect){x, 2, x + w, 2 + t->bh};
}

/* The bitmap and its column for image number `image`. */
static bool tb_image(toolbar *t, int32_t image, uint32_t *bitmap, int32_t *x)
{
    for (int k = t->nbitmaps - 1; k >= 0; k--) {
        if (image >= t->bitmap_base[k]) {
            *bitmap = t->bitmaps[k];
            *x = (image - t->bitmap_base[k]) * t->iw;
            return true;
        }
    }
    return false;
}

static void tb_paint(rc_cpu *c, uint32_t hwnd, toolbar *t)
{
    uint32_t hdc;
    rc_dc *d = paint_dc(c, hwnd, &hdc);
    if (!d) {
        return;
    }
    rc_dc_fill(d, rc_wnd_client(hwnd), rc_sys_color(15));
    for (int i = 0; i < t->n; i++) {
        tbutton *b = &t->b[i];
        if (b->state & 0x8) {
            continue;
        }
        rc_rect r = tbtn_rect(t, i);
        if (b->style & 1) { /* separator */
            int32_t mx = (r.l + r.r) / 2;
            rc_draw_edge(d, (rc_rect){mx - 1, r.t, mx + 1, r.b}, 0x6, 0x1 | 0x4);
            continue;
        }
        bool enabled = b->state & 0x4, down = (b->state & 0x3) || (i == t->pressed && t->tracking);
        if (b->state & 0x1 && !(i == t->pressed && t->tracking)) {
            rc_dc_fill(d, (rc_rect){r.l + 1, r.t + 1, r.r - 1, r.b - 1}, 0xe8e8e8);
        }
        rc_draw_edge(d, r, down ? 0xa : 0x5, 0xf);
        uint32_t bm;
        int32_t sx;
        if (tb_image(t, b->image, &bm, &sx)) {
            gobj *o = rc_gobj_get(bm, OB_BITMAP);
            if (o && o->pixels) {
                rc_dc src;
                memset(&src, 0, sizeof src);
                rc_gdi_bitmap_pixels(o, &src.target);
                src.vis.n = -1;
                if (o->ncolors) {
                    for (uint32_t k = 0; k < o->ncolors; k++) {
                        src.palette[k] = o->colors[k];
                    }
                    src.has_palette = 1;
                }
                int32_t ox = r.l + (t->bw - t->iw) / 2 + (down ? 1 : 0), oy = r.t + (t->bh - t->ih) / 2 + (down ? 1 : 0);
                uint32_t face = rc_sys_color(15) & 0xffffff;
                for (int32_t y = 0; y < t->ih && y < (int32_t)o->h; y++) {
                    for (int32_t x = 0; x < t->iw && sx + x < (int32_t)o->w; x++) {
                        uint32_t ref = rc_dc_colorref(&src, rc_dc_read(&src, sx + x, y)) & 0xffffff;
                        if (ref == face || ref == 0xc0c0c0) {
                            continue;
                        }
                        if (!enabled) {
                            /* disabled: the picture's dark parts embossed in the shadow color */
                            uint32_t l = ((ref & 0xff) + ((ref >> 8) & 0xff) + ((ref >> 16) & 0xff)) / 3;
                            if (l > 0xa0) {
                                continue;
                            }
                            rc_dc_plot(d, ox + x + 1, oy + y + 1, rc_dc_pixel(d, rc_sys_color(20)));
                            ref = rc_sys_color(16);
                        }
                        rc_dc_plot(d, ox + x, oy + y, rc_dc_pixel(d, ref));
                    }
                }
            }
        }
    }
    rc_wnd_release_dc(hdc);
}

static int tb_index(toolbar *t, uint32_t id)
{
    for (int i = 0; i < t->n; i++) {
        if (t->b[i].id == id) {
            return i;
        }
    }
    return -1;
}

static void tb_insert(toolbar *t, int at, uint32_t p)
{
    if (t->n == t->cap) {
        t->cap = t->cap ? 2 * t->cap : 16;
        t->b = realloc(t->b, sizeof(tbutton) * (size_t)t->cap);
    }
    if (at < 0 || at > t->n) {
        at = t->n;
    }
    memmove(&t->b[at + 1], &t->b[at], sizeof(tbutton) * (size_t)(t->n - at));
    t->b[at] = (tbutton){(int32_t)rc_ld32(p), rc_ld32(p + 4), rc_ld8(p + 8), rc_ld8(p + 9), rc_ld32(p + 12),
                         (int32_t)rc_ld32(p + 16)};
    t->n++;
}

static void put_button(uint32_t p, const tbutton *b)
{
    rc_st32(p, (uint32_t)b->image);
    rc_st32(p + 4, b->id);
    rc_st8(p + 8, b->state);
    rc_st8(p + 9, b->style);
    rc_st16(p + 10, 0);
    rc_st32(p + 12, b->data);
    rc_st32(p + 16, (uint32_t)b->string);
}

static int tb_at(toolbar *t, int32_t x, int32_t y)
{
    for (int i = 0; i < t->n; i++) {
        rc_rect r = tbtn_rect(t, i);
        if (!(t->b[i].state & 0x8) && !(t->b[i].style & 1) && x >= r.l && x < r.r && y >= r.t && y < r.b) {
            return i;
        }
    }
    return -1;
}

static void wp_toolbar(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), msg = RC_ARG(c, 1), wp = RC_ARG(c, 2), lp = RC_ARG(c, 3);
    toolbar *t = tbd(hwnd);
    int i;
    switch (msg) {
    case 0x0001: /* WM_CREATE */
        t->owner = rc_wnd_parent(hwnd);
        RET(0);
    case WM_PAINT:
        tb_paint(c, hwnd, t);
        RET(0);
    case WM_ERASEBKGND:
        RET(1);
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        i = tb_at(t, (int16_t)(lp & 0xffff), (int16_t)(lp >> 16));
        if (i >= 0 && (t->b[i].state & 0x4)) {
            t->pressed = i;
            t->tracking = true;
            rc_wnd_set_capture(hwnd);
            rc_wnd_invalidate(hwnd, NULL, true);
        }
        RET(0);
    case WM_LBUTTONUP:
        if (t->tracking) {
            t->tracking = false;
            rc_wnd_set_capture(0);
            i = tb_at(t, (int16_t)(lp & 0xffff), (int16_t)(lp >> 16));
            if (i >= 0 && i == t->pressed) {
                tbutton *b = &t->b[i];
                if (b->style & 2) { /* TBSTYLE_CHECK */
                    if ((b->style & 4) && !(b->state & 1)) { /* a check group: the others go off */
                        for (int k = 0; k < t->n; k++) {
                            if ((t->b[k].style & 6) == 6) {
                                t->b[k].state &= 0xfeu;
                            }
                        }
                        b->state |= 1;
                    } else if (!(b->style & 4)) {
                        b->state ^= 1;
                    }
                }
                rc_wnd_invalidate(hwnd, NULL, true);
                rc_wnd_send(c, t->owner ? t->owner : rc_wnd_parent(hwnd), WM_COMMAND, b->id & 0xffff, hwnd);
                RET(0);
            }
            t->pressed = -1;
            rc_wnd_invalidate(hwnd, NULL, true);
        }
        RET(0);
    case 0x401: /* TB_ENABLEBUTTON */
    case 0x402: /* TB_CHECKBUTTON */
    case 0x403: /* TB_PRESSBUTTON */
    case 0x404: /* TB_HIDEBUTTON */
    case 0x405: { /* TB_INDETERMINATE */
        static const uint8_t bit[5] = {0x4, 0x1, 0x2, 0x8, 0x10};
        i = tb_index(t, wp);
        if (i < 0) {
            RET(0);
        }
        uint8_t m = bit[msg - 0x401];
        t->b[i].state = (lp & 0xffff) ? t->b[i].state | m : t->b[i].state & (uint8_t)~m;
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(1);
    }
    case 0x409: case 0x40a: case 0x40b: case 0x40c: case 0x40d: { /* TB_IS* */
        static const uint8_t bit[5] = {0x4, 0x1, 0x2, 0x8, 0x10};
        i = tb_index(t, wp);
        RET(i >= 0 && (t->b[i].state & bit[msg - 0x409]) ? 1 : 0);
    }
    case 0x411: /* TB_SETSTATE */
        i = tb_index(t, wp);
        if (i >= 0 && t->b[i].state != (uint8_t)lp) {
            t->b[i].state = (uint8_t)lp;
            rc_wnd_invalidate(hwnd, NULL, true);
        }
        RET(i >= 0);
    case 0x412: /* TB_GETSTATE */
        i = tb_index(t, wp);
        RET(i >= 0 ? t->b[i].state : 0xffffffffu);
    case 0x413: { /* TB_ADDBITMAP(n, TBADDBITMAP: instance, id) */
        uint32_t inst = rc_ld32(lp), id = rc_ld32(lp + 4), bm = id;
        if (inst && inst != 0xffffffffu) {
            bm = rc_load_bitmap(inst, id); /* a resource, loaded as LoadBitmap does */
        } else if (inst == 0xffffffffu) {
            bm = 0; /* the standard images: none here */
        }
        int32_t base = t->images;
        if (t->nbitmaps < 16) {
            t->bitmaps[t->nbitmaps] = bm;
            t->bitmap_base[t->nbitmaps++] = base;
        }
        t->images += (int32_t)wp;
        RET((uint32_t)base);
    }
    case 0x42e: /* TB_REPLACEBITMAP(TBREPLACEBITMAP: old inst, old id, new inst, new id, n) */
        for (int k = 0; k < t->nbitmaps; k++) {
            if (t->bitmaps[k] == rc_ld32(lp + 4)) {
                t->bitmaps[k] = rc_ld32(lp + 12);
            }
        }
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(1);
    case 0x414: /* TB_ADDBUTTONSA(n, buttons) */
        for (uint32_t k = 0; k < wp; k++) {
            tb_insert(t, -1, lp + 20 * k);
        }
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(1);
    case 0x415: /* TB_INSERTBUTTONA(index, button) */
        tb_insert(t, (int)wp, lp);
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(1);
    case 0x416: /* TB_DELETEBUTTON */
        if ((int)wp < t->n) {
            memmove(&t->b[wp], &t->b[wp + 1], sizeof(tbutton) * (size_t)(t->n - (int)wp - 1));
            t->n--;
            rc_wnd_invalidate(hwnd, NULL, true);
            RET(1);
        }
        RET(0);
    case 0x417: /* TB_GETBUTTON(index, button) */
        if ((int)wp < t->n) {
            put_button(lp, &t->b[wp]);
            RET(1);
        }
        RET(0);
    case 0x418: RET((uint32_t)t->n);                              /* TB_BUTTONCOUNT */
    case 0x419: RET((uint32_t)tb_index(t, wp));                   /* TB_COMMANDTOINDEX */
    case 0x41d:                                                   /* TB_GETITEMRECT(index, rect) */
    case 0x433: {                                                 /* TB_GETRECT(id, rect) */
        i = msg == 0x433 ? tb_index(t, wp) : (int)wp;
        if (i < 0 || i >= t->n) {
            RET(0);
        }
        rc_rect r = tbtn_rect(t, i);
        rc_st32(lp, (uint32_t)r.l);
        rc_st32(lp + 4, (uint32_t)r.t);
        rc_st32(lp + 8, (uint32_t)r.r);
        rc_st32(lp + 12, (uint32_t)r.b);
        RET(1);
    }
    case 0x41e: RET(0);                                           /* TB_BUTTONSTRUCTSIZE */
    case 0x41f: t->bw = (int16_t)(lp & 0xffff); t->bh = (int16_t)(lp >> 16); rc_wnd_invalidate(hwnd, NULL, true); RET(1);
    case 0x420: t->iw = (int16_t)(lp & 0xffff); t->ih = (int16_t)(lp >> 16); RET(1);
    case 0x43a: RET(((uint32_t)t->bh << 16) | (uint32_t)t->bw);  /* TB_GETBUTTONSIZE */
    case 0x421: RET(0);                                           /* TB_AUTOSIZE */
    case 0x423: RET(0);                                           /* TB_GETTOOLTIPS */
    case 0x424: RET(0);                                           /* TB_SETTOOLTIPS */
    case 0x425: { uint32_t o = t->owner; t->owner = wp; RET(o); } /* TB_SETPARENT */
    case 0x427: RET(0);                                           /* TB_SETROWS */
    case 0x428: RET(1);                                           /* TB_GETROWS */
    case 0x42a:                                                   /* TB_SETCMDID(index, id) */
        if ((int)wp < t->n) {
            t->b[wp].id = lp;
        }
        RET(1);
    case 0x42b: /* TB_CHANGEBITMAP(id, image) */
        i = tb_index(t, wp);
        if (i >= 0) {
            t->b[i].image = (int32_t)lp;
            rc_wnd_invalidate(hwnd, NULL, true);
        }
        RET(i >= 0);
    case 0x42c: i = tb_index(t, wp); RET(i >= 0 ? (uint32_t)t->b[i].image : 0); /* TB_GETBITMAP */
    case 0x42f: t->indent = (int32_t)wp; RET(1);                                 /* TB_SETINDENT */
    case 0x41c: RET(0);                                                         /* TB_ADDSTRINGA */
    case 0x42d: RET(0xffffffffu);                                               /* TB_GETBUTTONTEXTA */
    case 0x438: rc_wnd_set_style(hwnd, lp); RET(0);                            /* TB_SETSTYLE */
    case 0x439: RET(rc_wnd_style(hwnd));                                       /* TB_GETSTYLE */
    case 0x41a: case 0x41b: case 0x430: case 0x431: case 0x43b: case 0x43c: RET(0);
    case 0x43d: RET(0);
    case WM_DESTROY:
        free(t->b);
        t->b = NULL;
        t->n = t->cap = 0;
        RET(0);
    default:
        RET(rc_def_window_proc(c, hwnd, msg, wp, lp));
    }
}

/* ---- image lists: kept as the bitmap they were made from ---- */

static void k_ImageList_Create(rc_cpu *c) { RC_RETURN(c, 0x00ab1000u, 5); }
static void k_ImageList_Destroy(rc_cpu *c) { RC_RETURN(c, 1, 1); }

static void k_InitCommonControls(rc_cpu *c) { RC_RETURN(c, 0, 0); }

/* CreateMappedBitmap(instance, id, flags, color map, count) (ordinal 8): the bitmap resource;
 * its colours are left as they are (the system colours it would map them to are the same here). */
static void k_CreateMappedBitmap(rc_cpu *c) { RC_RETURN(c, rc_load_bitmap(RC_ARG(c, 0), RC_ARG(c, 1)), 5); }
static void k_InitCommonControlsEx(rc_cpu *c) { RC_RETURN(c, 1, 1); }
/* _TrackMouseEvent(TRACKMOUSEEVENT): accepted (no WM_MOUSELEAVE is sent). */
static void k_TrackMouseEvent(rc_cpu *c) { RC_RETURN(c, 1, 1); }

/* DllGetVersion(DLLVERSIONINFO: size, major, minor, build, platform): 4.71, as Internet Explorer 4's. */
static void k_DllGetVersion(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    rc_st32(p + 4, 4);
    rc_st32(p + 8, 71);
    rc_st32(p + 12, 3110);
    rc_st32(p + 16, 1); /* DLLVER_PLATFORM_WINDOWS */
    RC_RETURN(c, 0, 1);
}

/* ---- tab control ---- */

#define TABS 32
typedef struct tabctl {
    struct {
        char *text;
        uint32_t param;
        int32_t image;
    } item[TABS];
    int n, sel;
    uint32_t images;
    int32_t width[TABS]; /* computed when painting or measuring */
} tabctl;

#define TCN_SELCHANGE 0xfffffdd9u   /* -551 */
#define TCN_SELCHANGING 0xfffffdd8u /* -552 */

static int32_t tab_height(rc_cpu *c, uint32_t hwnd) { return font_h(c, hwnd) + 8; }

/* The rectangle of tab `i` (client coordinates): one row from the left. */
static rc_rect tab_rect(rc_cpu *c, uint32_t hwnd, tabctl *t, int i)
{
    uint32_t hdc;
    rc_dc *d = paint_dc(c, hwnd, &hdc);
    int32_t x = 2, h = tab_height(c, hwnd);
    rc_rect r = {0, 0, 0, 0};
    for (int k = 0; k < t->n && k <= i; k++) {
        int32_t w = 16;
        if (d && t->item[k].text) {
            rc_rect m = {0, 0, 0, 0};
            rc_draw_text(d, t->item[k].text, -1, &m, 0x420 /* CALCRECT SINGLELINE */);
            w = m.r - m.l + 12;
        }
        t->width[k] = w;
        r = (rc_rect){x, 2, x + w, 2 + h};
        x += w;
    }
    rc_wnd_release_dc(hdc);
    return r;
}

static uint32_t tab_notify(rc_cpu *c, uint32_t hwnd, uint32_t code)
{
    uint32_t parent = rc_wnd_parent(hwnd);
    if (!parent) {
        return 0;
    }
    uint32_t nm = rc_heap_alloc(12, false);
    rc_st32(nm, hwnd);
    rc_st32(nm + 4, rc_wnd_id(hwnd));
    rc_st32(nm + 8, code);
    uint32_t r = rc_wnd_send(c, parent, 0x004e /* WM_NOTIFY */, rc_wnd_id(hwnd), nm);
    rc_heap_free(nm);
    return r;
}

static void tab_paint(rc_cpu *c, uint32_t hwnd, tabctl *t)
{
    uint32_t hdc;
    rc_dc *d = paint_dc(c, hwnd, &hdc);
    if (!d) {
        return;
    }
    rc_rect r = rc_wnd_client(hwnd);
    int32_t h = tab_height(c, hwnd);
    rc_dc_fill(d, r, rc_sys_color(15));
    rc_draw_edge(d, (rc_rect){0, h + 1, r.r, r.b}, 0x5 /* EDGE_RAISED */, 0xf);
    for (int i = 0; i < t->n; i++) {
        rc_rect tr = tab_rect(c, hwnd, t, i);
        if (i == t->sel) {
            tr.l -= 2;
            tr.r += 2;
            tr.t -= 2;
            tr.b += 2;
        }
        rc_dc_fill(d, tr, rc_sys_color(15));
        rc_draw_edge(d, tr, 0x5, 0x1 | 0x2 | 0x4 /* left top right */);
        if (t->item[i].text) {
            rc_draw_text(d, t->item[i].text, -1, &tr, 0x25 /* CENTER VCENTER SINGLELINE */);
        }
    }
    rc_wnd_release_dc(hdc);
}

static void tab_set_item(tabctl *t, int i, uint32_t item)
{
    uint32_t mask = rc_ld32(item);
    if (mask & 1) { /* TCIF_TEXT */
        free(t->item[i].text);
        uint32_t s = rc_ld32(item + 12);
        t->item[i].text = strdup(s ? rc_str(s) : "");
    }
    if (mask & 2) { /* TCIF_IMAGE */
        t->item[i].image = (int32_t)rc_ld32(item + 20);
    }
    if (mask & 8) { /* TCIF_PARAM */
        t->item[i].param = rc_ld32(item + 24);
    }
}

static void wp_tab(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), msg = RC_ARG(c, 1), wp = RC_ARG(c, 2), lp = RC_ARG(c, 3);
    tabctl *t = data_of(hwnd, sizeof *t);
    switch (msg) {
    case WM_PAINT:
        tab_paint(c, hwnd, t);
        RET(0);
    case WM_ERASEBKGND:
        RET(1);
    case WM_DESTROY:
        for (int i = 0; i < t->n; i++) {
            free(t->item[i].text);
        }
        free(t);
        rc_wnd_set_data(hwnd, NULL);
        RET(0);
    case WM_LBUTTONDOWN: {
        int32_t x = (int16_t)(lp & 0xffff), y = (int16_t)(lp >> 16);
        for (int i = 0; i < t->n; i++) {
            rc_rect r = tab_rect(c, hwnd, t, i);
            if (x >= r.l && x < r.r && y >= r.t && y < r.b && i != t->sel) {
                if (tab_notify(c, hwnd, TCN_SELCHANGING)) {
                    break; /* the parent keeps the current tab */
                }
                t->sel = i;
                rc_wnd_invalidate(hwnd, NULL, true);
                tab_notify(c, hwnd, TCN_SELCHANGE);
                break;
            }
        }
        RET(0);
    }
    case 0x1302: RET(t->images);                             /* TCM_GETIMAGELIST */
    case 0x1303: { uint32_t o = t->images; t->images = lp; RET(o); } /* TCM_SETIMAGELIST */
    case 0x1304: RET((uint32_t)t->n);                        /* TCM_GETITEMCOUNT */
    case 0x1305: {                                           /* TCM_GETITEMA(i, item) */
        if ((int)wp >= t->n || !lp) {
            RET(0);
        }
        uint32_t mask = rc_ld32(lp);
        if ((mask & 1) && rc_ld32(lp + 12)) {
            rc_put_str(rc_ld32(lp + 12), rc_ld32(lp + 16), t->item[wp].text ? t->item[wp].text : "");
        }
        if (mask & 2) {
            rc_st32(lp + 20, (uint32_t)t->item[wp].image);
        }
        if (mask & 8) {
            rc_st32(lp + 24, t->item[wp].param);
        }
        RET(1);
    }
    case 0x1306:                                             /* TCM_SETITEMA(i, item) */
        if ((int)wp >= t->n || !lp) {
            RET(0);
        }
        tab_set_item(t, (int)wp, lp);
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(1);
    case 0x1307: {                                           /* TCM_INSERTITEMA(i, item) */
        if (t->n == TABS || !lp) {
            RET(0xffffffffu);
        }
        int i = (int)wp > t->n ? t->n : (int)wp;
        memmove(&t->item[i + 1], &t->item[i], sizeof t->item[0] * (size_t)(t->n - i));
        memset(&t->item[i], 0, sizeof t->item[i]);
        t->item[i].image = -1;
        t->n++;
        tab_set_item(t, i, lp);
        if (t->n == 1) {
            t->sel = 0;
        } else if (i <= t->sel) {
            t->sel++;
        }
        rc_wnd_invalidate(hwnd, NULL, true);
        RET((uint32_t)i);
    }
    case 0x1308:                                             /* TCM_DELETEITEM(i) */
        if ((int)wp >= t->n) {
            RET(0);
        }
        free(t->item[wp].text);
        memmove(&t->item[wp], &t->item[wp + 1], sizeof t->item[0] * (size_t)(t->n - (int)wp - 1));
        t->n--;
        if (t->sel >= t->n) {
            t->sel = t->n - 1;
        }
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(1);
    case 0x1309:                                             /* TCM_DELETEALLITEMS */
        for (int i = 0; i < t->n; i++) {
            free(t->item[i].text);
        }
        t->n = 0;
        t->sel = -1;
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(1);
    case 0x130a: {                                           /* TCM_GETITEMRECT(i, rect) */
        if ((int)wp >= t->n || !lp) {
            RET(0);
        }
        rc_rect r = tab_rect(c, hwnd, t, (int)wp);
        rc_st32(lp, (uint32_t)r.l);
        rc_st32(lp + 4, (uint32_t)r.t);
        rc_st32(lp + 8, (uint32_t)r.r);
        rc_st32(lp + 12, (uint32_t)r.b);
        RET(1);
    }
    case 0x130b: RET((uint32_t)t->sel);                      /* TCM_GETCURSEL */
    case 0x132f: RET((uint32_t)t->sel);                      /* TCM_GETCURFOCUS */
    case 0x130c:                                             /* TCM_SETCURSEL(i) */
    case 0x1330: {                                           /* TCM_SETCURFOCUS(i) */
        int old = t->sel;
        if ((int)wp < t->n) {
            t->sel = (int)wp;
            rc_wnd_invalidate(hwnd, NULL, true);
        }
        RET((uint32_t)old);
    }
    case 0x130d: {                                           /* TCM_HITTEST(0, info) */
        if (!lp) {
            RET(0xffffffffu);
        }
        int32_t x = (int32_t)rc_ld32(lp), y = (int32_t)rc_ld32(lp + 4);
        for (int i = 0; i < t->n; i++) {
            rc_rect r = tab_rect(c, hwnd, t, i);
            if (x >= r.l && x < r.r && y >= r.t && y < r.b) {
                rc_st32(lp + 8, 6); /* TCHT_ONITEM */
                RET((uint32_t)i);
            }
        }
        rc_st32(lp + 8, 1); /* TCHT_NOWHERE */
        RET(0xffffffffu);
    }
    case 0x1328: {                                           /* TCM_ADJUSTRECT(larger, rect) */
        int32_t h = tab_height(c, hwnd) + 4;
        if (lp) {
            int32_t d = wp ? -1 : 1;
            rc_st32(lp, rc_ld32(lp) + (uint32_t)(2 * d));
            rc_st32(lp + 4, rc_ld32(lp + 4) + (uint32_t)(h * d));
            rc_st32(lp + 8, rc_ld32(lp + 8) - (uint32_t)(2 * d));
            rc_st32(lp + 12, rc_ld32(lp + 12) - (uint32_t)(2 * d));
        }
        RET(0);
    }
    case 0x132c: RET(1);                                     /* TCM_GETROWCOUNT */
    case 0x130e:                                             /* TCM_SETITEMEXTRA */
    case 0x1329:                                             /* TCM_SETITEMSIZE */
    case 0x132b:                                             /* TCM_SETPADDING */
    case 0x1331:                                             /* TCM_SETMINTABWIDTH */
    case 0x132d:                                             /* TCM_GETTOOLTIPS */
        RET(0);
    default:
        RET(rc_def_window_proc(c, hwnd, msg, wp, lp));
    }
}

static const rc_import_def defs[] = {
    {CC, "DllGetVersion", k_DllGetVersion},
    {CC, "#17", k_InitCommonControls},
    {CC, "#8", k_CreateMappedBitmap},
    {CC, "CreateMappedBitmap", k_CreateMappedBitmap},
    {CC, "InitCommonControls", k_InitCommonControls},
    {CC, "InitCommonControlsEx", k_InitCommonControlsEx},
    {CC, "_TrackMouseEvent", k_TrackMouseEvent},
    {CC, "ImageList_Create", k_ImageList_Create},
    {CC, "ImageList_Destroy", k_ImageList_Destroy},
};

void rc_comctl32_init(void)
{
    rc_register_imports(defs, sizeof defs / sizeof defs[0]);
    uint32_t arrow = rc_arrow_cursor();
    rc_class_builtin("msctls_statusbar32", 0x8, wp_statusbar, 8, 0, arrow);
    rc_class_builtin("msctls_progress32", 0x8, wp_progress, 8, 0, arrow);
    rc_class_builtin("msctls_trackbar32", 0x8, wp_trackbar, 8, 0, arrow);
    rc_class_builtin("tooltips_class32", 0x8, wp_tooltips, 8, 0, arrow);
    rc_class_builtin("ToolbarWindow32", 0x8, wp_toolbar, 8, 0, arrow);
    rc_class_builtin("SysTabControl32", 0x8, wp_tab, 8, 0, arrow);
}
