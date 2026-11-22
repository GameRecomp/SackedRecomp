/* SPDX-License-Identifier: MIT */
/*
 * user32: the standard controls (BUTTON, STATIC, EDIT, LISTBOX, COMBOBOX, SCROLLBAR) as window
 * procedures of our own, and scroll bars (the ones in a window's frame and the control). Each
 * control keeps its state as host data on its window; it tells its parent what happens with
 * WM_COMMAND, asks it for colors (WM_CTLCOLOR*) and lets it draw owner-drawn items (WM_DRAWITEM).
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdi32/dc.h"
#include "gdi32/objects.h"
#include "rc/hle.h"
#include "rc/log.h"
#include "rc/platform.h"
#include "rc/process.h"
#include "rc/text.h"
#include "rc/thread.h"
#include "rc/vm.h"
#include "user32.h"

#define U32 "USER32.dll"

#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_SIZE 0x0005u
#define WM_SETFOCUS 0x0007u
#define WM_KILLFOCUS 0x0008u
#define WM_ENABLE 0x000au
#define WM_SETTEXT 0x000cu
#define WM_GETTEXT 0x000du
#define WM_GETTEXTLENGTH 0x000eu
#define WM_PAINT 0x000fu
#define WM_ERASEBKGND 0x0014u
#define WM_SETFONT 0x0030u
#define WM_GETFONT 0x0031u
#define WM_DRAWITEM 0x002bu
#define WM_MEASUREITEM 0x002cu
#define WM_NCHITTEST 0x0084u
#define WM_GETDLGCODE 0x0087u
#define WM_KEYDOWN 0x0100u
#define WM_CHAR 0x0102u
#define WM_COMMAND 0x0111u
#define WM_HSCROLL 0x0114u
#define WM_VSCROLL 0x0115u
#define WM_CTLCOLOREDIT 0x0133u
#define WM_CTLCOLORLISTBOX 0x0134u
#define WM_CTLCOLORBTN 0x0135u
#define WM_CTLCOLORSCROLLBAR 0x0137u
#define WM_CTLCOLORSTATIC 0x0138u
#define WM_MOUSEMOVE 0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP 0x0202u
#define WM_LBUTTONDBLCLK 0x0203u
#define WM_MOUSEWHEEL 0x020au
#define WM_CAPTURECHANGED 0x0215u

#define DLGC_WANTARROWS 0x1u
#define DLGC_WANTTAB 0x2u
#define DLGC_WANTALLKEYS 0x4u
#define DLGC_HASSETSEL 0x8u
#define DLGC_DEFPUSHBUTTON 0x10u
#define DLGC_UNDEFPUSHBUTTON 0x20u
#define DLGC_RADIOBUTTON 0x40u
#define DLGC_WANTCHARS 0x80u
#define DLGC_STATIC 0x100u
#define DLGC_BUTTON 0x2000u

static void ret(rc_cpu *c, uint32_t v) { RC_RETURN(c, v, 4); }
#define RET(v)                                                                                                         \
    do {                                                                                                               \
        ret(c, (v));                                                                                                   \
        return;                                                                                                        \
    } while (0)

static void notify(rc_cpu *c, uint32_t hwnd, uint32_t code)
{
    uint32_t parent = rc_wnd_parent(hwnd);
    if (parent) {
        rc_wnd_send(c, parent, WM_COMMAND, (rc_wnd_id(hwnd) & 0xffff) | (code << 16), hwnd);
    }
}

/* A DC to paint a control with: its font, colors from the parent (WM_CTLCOLOR*): the
 * background as a COLORREF in *bg (none: false). */
typedef struct paint {
    uint32_t hdc;
    rc_dc *d;
    uint32_t bg;
    bool has_bg;
} paint;

static bool begin(rc_cpu *c, uint32_t hwnd, uint32_t ctlcolor, paint *p)
{
    rc_wnd_validate(hwnd);
    p->hdc = rc_wnd_dc(hwnd, false);
    p->d = rc_dc_get(p->hdc);
    if (!p->d) {
        return false;
    }
    uint32_t font = rc_def_window_proc(c, hwnd, WM_GETFONT, 0, 0);
    p->d->font = font ? font : rc_gui_font();
    p->d->bk_mode = 2;
    uint32_t parent = rc_wnd_parent(hwnd);
    uint32_t brush = parent ? rc_wnd_send(c, parent, ctlcolor, p->hdc, hwnd) : 0;
    if (!brush) {
        brush = rc_def_window_proc(c, hwnd, ctlcolor, p->hdc, hwnd);
    }
    p->has_bg = rc_brush_color(brush, &p->bg);
    if (!p->has_bg) {
        p->bg = rc_sys_color(15);
    }
    return true;
}

static void end(paint *p) { rc_wnd_release_dc(p->hdc); }

/* ---- scroll bars ---- */

static int32_t sb_range(const rc_scroll *s) { return s->max - s->min + 1; }

/* The thumb within the track (pixels along the bar). */
static void thumb_of(const rc_scroll *s, int32_t track, int32_t *at, int32_t *len)
{
    int32_t range = sb_range(s);
    if (range <= 0 || (s->page && s->page >= range)) {
        *at = 0;
        *len = 0;
        return;
    }
    *len = s->page ? (int32_t)((int64_t)track * s->page / range) : 16;
    if (*len < 8) {
        *len = 8;
    }
    if (*len > track) {
        *len = track;
    }
    int32_t span = range - (s->page ? s->page : 1);
    *at = span > 0 ? (int32_t)((int64_t)(track - *len) * (s->pos - s->min) / span) : 0;
    if (*at < 0) {
        *at = 0;
    }
}

static void arrow(rc_dc *d, rc_rect r, int dir, bool enabled) /* 0 up 1 down 2 left 3 right */
{
    rc_dc_fill(d, r, rc_sys_color(15));
    rc_draw_edge(d, r, 0x5, 0xf);
    uint32_t color = rc_sys_color(enabled ? 18 : 17);
    int32_t cx = (r.l + r.r) / 2, cy = (r.t + r.b) / 2;
    for (int k = 0; k < 4; k++) {
        switch (dir) {
        case 0: rc_dc_fill(d, (rc_rect){cx - k, cy - 2 + k, cx + k + 1, cy - 1 + k}, color); break;
        case 1: rc_dc_fill(d, (rc_rect){cx - 3 + k, cy - 1 + k, cx + 4 - k, cy + k}, color); break;
        case 2: rc_dc_fill(d, (rc_rect){cx - 2 + k, cy - k, cx - 1 + k, cy + k + 1}, color); break;
        default: rc_dc_fill(d, (rc_rect){cx - 1 + k, cy - 3 + k, cx + k, cy + 4 - k}, color); break;
        }
    }
}

void rc_scroll_draw(rc_dc *d, rc_rect r, bool vertical, const rc_scroll *s, bool enabled)
{
    int32_t len = vertical ? r.b - r.t : r.r - r.l, a = vertical ? r.r - r.l : r.b - r.t;
    if (a > len / 2) {
        a = len / 2;
    }
    rc_rect first = vertical ? (rc_rect){r.l, r.t, r.r, r.t + a} : (rc_rect){r.l, r.t, r.l + a, r.b};
    rc_rect last = vertical ? (rc_rect){r.l, r.b - a, r.r, r.b} : (rc_rect){r.r - a, r.t, r.r, r.b};
    rc_rect track = vertical ? (rc_rect){r.l, r.t + a, r.r, r.b - a} : (rc_rect){r.l + a, r.t, r.r - a, r.b};
    bool on = enabled && !s->disabled && sb_range(s) > (s->page ? s->page : 1);
    rc_dc_fill(d, track, 0xe0e0e0);
    arrow(d, first, vertical ? 0 : 2, on);
    arrow(d, last, vertical ? 1 : 3, on);
    if (on) {
        int32_t at, tl;
        thumb_of(s, vertical ? track.b - track.t : track.r - track.l, &at, &tl);
        if (tl) {
            rc_rect th = vertical ? (rc_rect){track.l, track.t + at, track.r, track.t + at + tl}
                                  : (rc_rect){track.l + at, track.t, track.l + at + tl, track.b};
            rc_dc_fill(d, th, rc_sys_color(15));
            rc_draw_edge(d, th, 0x5, 0xf);
        }
    }
}

static void scroll_send(rc_cpu *c, uint32_t to, uint32_t ctl, bool vertical, uint32_t code, int32_t pos)
{
    rc_wnd_send(c, to, vertical ? WM_VSCROLL : WM_HSCROLL, code | ((uint32_t)(pos & 0xffff) << 16), ctl);
}

void rc_scroll_track(rc_cpu *c, uint32_t to, uint32_t ctl, rc_rect r, bool vertical, rc_scroll *s, int32_t x, int32_t y)
{
    if (s->disabled || sb_range(s) <= (s->page ? s->page : 1)) {
        return;
    }
    int32_t len = vertical ? r.b - r.t : r.r - r.l, a = vertical ? r.r - r.l : r.b - r.t;
    if (a > len / 2) {
        a = len / 2;
    }
    int32_t along = vertical ? y - r.t : x - r.l, track = len - 2 * a;
    int32_t at, tl;
    thumb_of(s, track, &at, &tl);
    if (along < a) {
        scroll_send(c, to, ctl, vertical, 0, 0); /* SB_LINEUP */
    } else if (along >= len - a) {
        scroll_send(c, to, ctl, vertical, 1, 0); /* SB_LINEDOWN */
    } else if (along < a + at) {
        scroll_send(c, to, ctl, vertical, 2, 0); /* SB_PAGEUP */
    } else if (along >= a + at + tl) {
        scroll_send(c, to, ctl, vertical, 3, 0); /* SB_PAGEDOWN */
    } else {
        /* the thumb: SB_THUMBTRACK while it moves, SB_THUMBPOSITION where it is let go */
        int32_t grab = along - a - at, pos = s->pos, mx = x, my = y;
        int32_t span = sb_range(s) - (s->page ? s->page : 1);
        while (rc_input_track(c, &mx, &my)) {
            int32_t p = (vertical ? my - r.t : mx - r.l) - a - grab;
            pos = s->min + (track - tl > 0 ? (int32_t)((int64_t)p * span / (track - tl)) : 0);
            pos = pos < s->min ? s->min : pos > s->min + span ? s->min + span : pos;
            s->track = pos;
            scroll_send(c, to, ctl, vertical, 5, pos);
        }
        scroll_send(c, to, ctl, vertical, 4, pos);
    }
    scroll_send(c, to, ctl, vertical, 8, 0); /* SB_ENDSCROLL */
}

/* The scroll bar `bar` of a window (SB_CTL 2: a SCROLLBAR control's own). */
static rc_scroll *bar_of(uint32_t hwnd, uint32_t bar) { return rc_wnd_scroll(hwnd, bar == 2 ? 0 : (int)bar); }

/* After a change: a window's own bar appears or goes as the range needs; it is redrawn. */
static void bar_changed(rc_cpu *c, uint32_t hwnd, uint32_t bar, bool keep)
{
    if (bar == 2) {
        rc_wnd_invalidate(hwnd, NULL, false);
        return;
    }
    rc_scroll *s = bar_of(hwnd, bar);
    uint32_t flag = bar ? WS_VSCROLL : WS_HSCROLL, style = rc_wnd_style(hwnd);
    bool need = keep || sb_range(s) > (s->page ? s->page : 1);
    if (need != ((style & flag) != 0)) {
        rc_wnd_set_style(hwnd, need ? style | flag : style & ~flag);
        rc_wnd_frame_changed(c, hwnd);
    } else {
        rc_wnd_redraw_frame(hwnd);
    }
}

/* SetScrollInfo(hwnd, bar, info, redraw) */
static void k_SetScrollInfo(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), bar = RC_ARG(c, 1), p = RC_ARG(c, 2);
    rc_scroll *s = bar_of(hwnd, bar);
    if (!s) {
        RC_RETURN(c, 0, 4);
    }
    uint32_t mask = rc_ld32(p + 4);
    if (mask & 1) {
        s->min = (int32_t)rc_ld32(p + 8);
        s->max = (int32_t)rc_ld32(p + 12);
    }
    if (mask & 2) {
        s->page = (int32_t)rc_ld32(p + 16);
    }
    if (mask & 4) {
        s->pos = (int32_t)rc_ld32(p + 20);
    }
    int32_t top = s->max - (s->page ? s->page - 1 : 0);
    if (s->pos > top) {
        s->pos = top;
    }
    if (s->pos < s->min) {
        s->pos = s->min;
    }
    bar_changed(c, hwnd, bar, (mask & 8) != 0);
    RC_RETURN(c, (uint32_t)s->pos, 4);
}

static void k_GetScrollInfo(rc_cpu *c)
{
    rc_scroll *s = bar_of(RC_ARG(c, 0), RC_ARG(c, 1));
    uint32_t p = RC_ARG(c, 2);
    if (!s) {
        RC_RETURN(c, 0, 3);
    }
    uint32_t mask = rc_ld32(p + 4);
    if (mask & 1) {
        rc_st32(p + 8, (uint32_t)s->min);
        rc_st32(p + 12, (uint32_t)s->max);
    }
    if (mask & 2) {
        rc_st32(p + 16, (uint32_t)s->page);
    }
    if (mask & 4) {
        rc_st32(p + 20, (uint32_t)s->pos);
    }
    if (mask & 0x10) {
        rc_st32(p + 24, (uint32_t)s->track);
    }
    RC_RETURN(c, 1, 3);
}

static void k_SetScrollPos(rc_cpu *c)
{
    rc_scroll *s = bar_of(RC_ARG(c, 0), RC_ARG(c, 1));
    if (!s) {
        RC_RETURN(c, 0, 4);
    }
    int32_t old = s->pos;
    s->pos = (int32_t)RC_ARG(c, 2);
    if (RC_ARG(c, 1) == 2) {
        rc_wnd_invalidate(RC_ARG(c, 0), NULL, false);
    } else {
        rc_wnd_redraw_frame(RC_ARG(c, 0)); /* the position only: the bar stays as it is */
    }
    RC_RETURN(c, (uint32_t)old, 4);
}

static void k_GetScrollPos(rc_cpu *c)
{
    rc_scroll *s = bar_of(RC_ARG(c, 0), RC_ARG(c, 1));
    RC_RETURN(c, s ? (uint32_t)s->pos : 0, 2);
}

static void k_SetScrollRange(rc_cpu *c)
{
    rc_scroll *s = bar_of(RC_ARG(c, 0), RC_ARG(c, 1));
    if (s) {
        s->min = (int32_t)RC_ARG(c, 2);
        s->max = (int32_t)RC_ARG(c, 3);
        s->page = 0;
        bar_changed(c, RC_ARG(c, 0), RC_ARG(c, 1), false);
    }
    RC_RETURN(c, s != NULL, 5);
}

static void k_GetScrollRange(rc_cpu *c)
{
    rc_scroll *s = bar_of(RC_ARG(c, 0), RC_ARG(c, 1));
    rc_st32(RC_ARG(c, 2), s ? (uint32_t)s->min : 0);
    rc_st32(RC_ARG(c, 3), s ? (uint32_t)s->max : 0);
    RC_RETURN(c, s != NULL, 4);
}

/* ShowScrollBar(hwnd, bar, show): SB_BOTH 3 */
static void k_ShowScrollBar(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), bar = RC_ARG(c, 1), show = RC_ARG(c, 2);
    uint32_t flags = bar == 3 ? WS_HSCROLL | WS_VSCROLL : bar == 1 ? WS_VSCROLL : bar == 0 ? WS_HSCROLL : 0;
    if (flags) {
        uint32_t style = rc_wnd_style(hwnd), ns = show ? style | flags : style & ~flags;
        if (ns != style) {
            rc_wnd_set_style(hwnd, ns);
            rc_wnd_frame_changed(c, hwnd);
        }
    } else if (bar == 2) {
        rc_wnd_show(c, hwnd, show ? 5 : 0);
    }
    RC_RETURN(c, 1, 3);
}

static void k_EnableScrollBar(rc_cpu *c)
{
    uint32_t bar = RC_ARG(c, 1);
    for (uint32_t b = 0; b < 2; b++) {
        if (bar == 3 || bar == b || (bar == 2 && b == 0)) {
            rc_scroll *s = bar_of(RC_ARG(c, 0), b);
            if (s) {
                s->disabled = RC_ARG(c, 2) == 3; /* ESB_DISABLE_BOTH */
            }
        }
    }
    bar_changed(c, RC_ARG(c, 0), bar == 2 ? 2 : 0, true);
    RC_RETURN(c, 1, 3);
}

/* The SCROLLBAR control: SBS_VERT 1. */
static void wp_scrollbar(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), msg = RC_ARG(c, 1), wp = RC_ARG(c, 2), lp = RC_ARG(c, 3);
    rc_scroll *s = rc_wnd_scroll(hwnd, 0);
    bool vertical = rc_wnd_style(hwnd) & 1;
    switch (msg) {
    case WM_PAINT: {
        paint p;
        if (begin(c, hwnd, WM_CTLCOLORSCROLLBAR, &p)) {
            rc_scroll_draw(p.d, rc_wnd_client(hwnd), vertical, s, rc_wnd_enabled(hwnd));
            end(&p);
        }
        RET(0);
    }
    case WM_LBUTTONDOWN: {
        int32_t ox, oy;
        rc_wnd_client_origin(hwnd, &ox, &oy);
        rc_rect r = rc_wnd_client(hwnd);
        r.l += ox, r.r += ox, r.t += oy, r.b += oy;
        rc_scroll_track(c, rc_wnd_parent(hwnd), hwnd, r, vertical, s, (int16_t)(lp & 0xffff) + ox, (int16_t)(lp >> 16) + oy);
        RET(0);
    }
    case 0xe0: { /* SBM_SETPOS */
        int32_t old = s->pos;
        s->pos = (int32_t)wp;
        if (lp) {
            rc_wnd_invalidate(hwnd, NULL, false);
        }
        RET((uint32_t)old);
    }
    case 0xe1: RET((uint32_t)s->pos);
    case 0xe2:
    case 0xe6:
        s->min = (int32_t)wp;
        s->max = (int32_t)lp;
        rc_wnd_invalidate(hwnd, NULL, false);
        RET(1);
    case 0xe3:
        rc_st32(wp, (uint32_t)s->min);
        rc_st32(lp, (uint32_t)s->max);
        RET(1);
    case 0xe9: { /* SBM_SETSCROLLINFO */
        uint32_t mask = rc_ld32(lp + 4);
        if (mask & 1) {
            s->min = (int32_t)rc_ld32(lp + 8);
            s->max = (int32_t)rc_ld32(lp + 12);
        }
        if (mask & 2) {
            s->page = (int32_t)rc_ld32(lp + 16);
        }
        if (mask & 4) {
            s->pos = (int32_t)rc_ld32(lp + 20);
        }
        rc_wnd_invalidate(hwnd, NULL, false);
        RET((uint32_t)s->pos);
    }
    case 0xea:
        rc_st32(lp + 8, (uint32_t)s->min);
        rc_st32(lp + 12, (uint32_t)s->max);
        rc_st32(lp + 16, (uint32_t)s->page);
        rc_st32(lp + 20, (uint32_t)s->pos);
        rc_st32(lp + 24, (uint32_t)s->track);
        RET(1);
    case WM_ERASEBKGND: RET(1);
    default: RET(rc_def_window_proc(c, hwnd, msg, wp, lp));
    }
}

/* ---- BUTTON ---- */

typedef struct button {
    uint32_t check, state; /* BST_CHECKED 1, BST_INDETERMINATE 2; state: BST_PUSHED 4, BST_FOCUS 8 */
    bool tracking;
} button;

static button *btn(uint32_t hwnd)
{
    button *b = rc_wnd_data(hwnd);
    if (!b) {
        b = calloc(1, sizeof *b);
        rc_wnd_set_data(hwnd, b);
    }
    return b;
}

static uint32_t btype(uint32_t hwnd) { return rc_wnd_style(hwnd) & 0xf; }

static void draw_owner(rc_cpu *c, uint32_t hwnd, uint32_t hdc, uint32_t type, uint32_t item, uint32_t action,
                       uint32_t state, rc_rect r, uint32_t data)
{
    uint32_t dis = rc_heap_alloc(48, true);
    uint32_t v[12] = {type, rc_wnd_id(hwnd), item, action, state, hwnd, hdc, (uint32_t)r.l, (uint32_t)r.t,
                      (uint32_t)r.r, (uint32_t)r.b, data};
    for (int i = 0; i < 12; i++) {
        rc_st32(dis + 4 * (uint32_t)i, v[i]);
    }
    rc_wnd_send(c, rc_wnd_parent(hwnd), WM_DRAWITEM, rc_wnd_id(hwnd), dis);
    rc_heap_free(dis);
}

static void button_paint(rc_cpu *c, uint32_t hwnd)
{
    button *b = btn(hwnd);
    uint32_t t = btype(hwnd), style = rc_wnd_style(hwnd);
    bool push = t == 0 || t == 1, group = t == 7, radio = t == 4 || t == 9;
    paint p;
    if (!begin(c, hwnd, push ? WM_CTLCOLORBTN : WM_CTLCOLORSTATIC, &p)) {
        return;
    }
    rc_dc *d = p.d;
    rc_rect r = rc_wnd_client(hwnd);
    bool enabled = rc_wnd_enabled(hwnd), focus = rc_wnd_focus() == hwnd, pushed = b->state & 4;
    if (t == 0xb) { /* BS_OWNERDRAW */
        uint32_t state = (pushed ? 1 : 0) | (enabled ? 0 : 4) | (focus ? 0x10 : 0);
        draw_owner(c, hwnd, p.hdc, 4 /* ODT_BUTTON */, 0, 1 /* ODA_DRAWENTIRE */, state, r, 0);
        end(&p);
        return;
    }
    const char *text = rc_wnd_text(hwnd);
    d->bk_mode = 1;
    d->text_color = rc_sys_color(enabled ? 18 : 17);
    if (push) {
        rc_dc_fill(d, r, rc_sys_color(15));
        bool def = t == 1;
        rc_rect e = r;
        if (def) {
            rc_dc_fill(d, (rc_rect){r.l, r.t, r.r, r.t + 1}, 0);
            rc_dc_fill(d, (rc_rect){r.l, r.b - 1, r.r, r.b}, 0);
            rc_dc_fill(d, (rc_rect){r.l, r.t, r.l + 1, r.b}, 0);
            rc_dc_fill(d, (rc_rect){r.r - 1, r.t, r.r, r.b}, 0);
            e = (rc_rect){r.l + 1, r.t + 1, r.r - 1, r.b - 1};
        }
        if (pushed) {
            rc_draw_edge(d, e, 0, 0xf | 0x4000);
        } else {
            rc_draw_edge(d, e, 0x5, 0xf);
        }
        rc_rect tr = e;
        if (pushed) {
            tr.l += 1, tr.t += 1, tr.r += 1, tr.b += 1;
        }
        uint32_t fmt = 0x25 | (style & 0x2000 ? 0x10 : 0);
        if (style & 0x2000) { /* BS_MULTILINE */
            fmt = 0x11;
        }
        rc_draw_text(d, text, -1, &tr, fmt);
        if (focus) {
            rc_draw_focus(d, (rc_rect){e.l + 3, e.t + 3, e.r - 3, e.b - 3});
        }
    } else if (group) {
        rc_font *f = rc_gdi_font_of(d);
        rc_font_metrics m = {13, 3, 16, 0, 7, 14};
        if (f) {
            rc_font_metrics_of(f, &m);
        }
        int32_t ty = m.height / 2;
        rc_draw_edge(d, (rc_rect){r.l, r.t + ty, r.r, r.b}, 0x6 /* EDGE_ETCHED */, 0xf);
        rc_rect tr = {r.l + 8, r.t, r.r - 8, r.t + m.height};
        rc_rect calc = tr;
        rc_draw_text(d, text, -1, &calc, 0x420);
        if (*text) {
            rc_dc_fill(d, (rc_rect){tr.l - 2, tr.t, calc.r + 2, tr.b}, p.bg);
        }
        rc_draw_text(d, text, -1, &tr, 0x20);
    } else {
        /* check box or radio button: the box, then the text */
        if (p.has_bg) {
            rc_dc_fill(d, r, p.bg);
        }
        bool left_text = style & 0x20; /* BS_LEFTTEXT */
        int32_t cy = (r.t + r.b) / 2;
        rc_rect box = left_text ? (rc_rect){r.r - 13, cy - 6, r.r, cy + 7} : (rc_rect){r.l, cy - 6, r.l + 13, cy + 7};
        rc_dc_fill(d, box, rc_sys_color(enabled && !pushed ? 5 : 15));
        rc_draw_edge(d, box, 0xa, 0xf);
        uint32_t mark = rc_sys_color(enabled ? 8 : 17);
        if (b->check & 1) {
            if (radio) {
                rc_dc_fill(d, (rc_rect){box.l + 4, box.t + 4, box.r - 4, box.b - 4}, mark);
            } else {
                for (int k = 0; k < 3; k++) {
                    rc_dc_fill(d, (rc_rect){box.l + 3 + k, box.t + 5 + k, box.l + 4 + k, box.t + 8 + k}, mark);
                }
                for (int k = 0; k < 4; k++) {
                    rc_dc_fill(d, (rc_rect){box.l + 6 + k, box.t + 6 - k, box.l + 7 + k, box.t + 9 - k}, mark);
                }
            }
        } else if (b->check & 2) {
            rc_dc_fill(d, (rc_rect){box.l + 3, box.t + 3, box.r - 3, box.b - 3}, rc_sys_color(16));
        }
        rc_rect tr = left_text ? (rc_rect){r.l, r.t, r.r - 17, r.b} : (rc_rect){r.l + 17, r.t, r.r, r.b};
        rc_draw_text(d, text, -1, &tr, (style & 0x2000) ? 0x10 : 0x24);
        if (focus && *text) {
            rc_rect calc = tr;
            rc_draw_text(d, text, -1, &calc, 0x420);
            int32_t h = calc.b - calc.t, y = (tr.t + tr.b - h) / 2;
            rc_draw_focus(d, (rc_rect){tr.l - 1, y - 1, calc.r + 1, y + h + 1});
        }
    }
    end(&p);
}

static void button_click(rc_cpu *c, uint32_t hwnd)
{
    button *b = btn(hwnd);
    uint32_t t = btype(hwnd);
    if (t == 3) { /* BS_AUTOCHECKBOX */
        b->check = b->check & 1 ? 0 : 1;
    } else if (t == 6) { /* BS_AUTO3STATE */
        b->check = (b->check + 1) % 3;
    } else if (t == 9) { /* BS_AUTORADIOBUTTON: the others of its group go off */
        uint32_t parent = rc_wnd_parent(hwnd);
        for (uint32_t k = rc_wnd_first_child(parent); k; k = rc_wnd_next_sibling(k)) {
            if (k != hwnd && !strcmp(rc_wnd_class_name(k), "Button") && btype(k) == 9 && btn(k)->check) {
                btn(k)->check = 0;
                rc_wnd_invalidate(k, NULL, true);
            }
        }
        b->check = 1;
    }
    rc_wnd_invalidate(hwnd, NULL, true);
    notify(c, hwnd, 0 /* BN_CLICKED */);
}

static void wp_button(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), msg = RC_ARG(c, 1), wp = RC_ARG(c, 2), lp = RC_ARG(c, 3);
    button *b = btn(hwnd);
    uint32_t t = btype(hwnd);
    switch (msg) {
    case WM_PAINT:
        button_paint(c, hwnd);
        RET(0);
    case WM_ERASEBKGND:
        RET(1);
    case WM_GETDLGCODE:
        if (t == 0) {
            RET(DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON);
        }
        if (t == 1) {
            RET(DLGC_BUTTON | DLGC_DEFPUSHBUTTON);
        }
        if (t == 4 || t == 9) {
            RET(DLGC_BUTTON | DLGC_RADIOBUTTON);
        }
        RET(t == 7 ? DLGC_STATIC : DLGC_BUTTON);
    case WM_NCHITTEST:
        RET(t == 7 ? 0xffffffffu /* HTTRANSPARENT */ : rc_def_window_proc(c, hwnd, msg, wp, lp));
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (t == 7) {
            RET(0);
        }
        rc_wnd_set_focus(c, hwnd);
        rc_wnd_set_capture(hwnd);
        b->tracking = true;
        b->state |= 4;
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(0);
    case WM_MOUSEMOVE:
        if (b->tracking) {
            rc_rect r = rc_wnd_client(hwnd);
            int32_t x = (int16_t)(lp & 0xffff), y = (int16_t)(lp >> 16);
            uint32_t ns = (x >= r.l && x < r.r && y >= r.t && y < r.b) ? b->state | 4 : b->state & ~4u;
            if (ns != b->state) {
                b->state = ns;
                rc_wnd_invalidate(hwnd, NULL, true);
            }
        }
        RET(0);
    case WM_LBUTTONUP:
        if (b->tracking) {
            b->tracking = false;
            rc_wnd_set_capture(0);
            bool inside = b->state & 4;
            b->state &= ~4u;
            rc_wnd_invalidate(hwnd, NULL, true);
            if (inside) {
                button_click(c, hwnd);
            }
        }
        RET(0);
    case WM_KEYDOWN:
        if (wp == 0x20) {
            button_click(c, hwnd);
        }
        RET(0);
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        rc_wnd_invalidate(hwnd, NULL, true);
        if (msg == WM_SETFOCUS && (rc_wnd_style(hwnd) & 0x4000 /* BS_NOTIFY */)) {
            notify(c, hwnd, 6 /* BN_SETFOCUS */);
        }
        RET(0);
    case WM_ENABLE:
    case WM_SETTEXT: {
        uint32_t r = rc_def_window_proc(c, hwnd, msg, wp, lp);
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(r);
    }
    case 0xf0: RET(b->check);                      /* BM_GETCHECK */
    case 0xf1:                                               /* BM_SETCHECK */
        b->check = wp;
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(0);
    case 0xf2: RET(b->check | b->state | (rc_wnd_focus() == hwnd ? 8 : 0)); /* BM_GETSTATE */
    case 0xf3:                                                                  /* BM_SETSTATE */
        b->state = wp ? b->state | 4 : b->state & ~4u;
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(0);
    case 0xf4: /* BM_SETSTYLE */
        rc_wnd_set_style(hwnd, (rc_wnd_style(hwnd) & ~0xffffu) | (wp & 0xffff));
        if (lp) {
            rc_wnd_invalidate(hwnd, NULL, true);
        }
        RET(0);
    case 0xf5: /* BM_CLICK */
        button_click(c, hwnd);
        RET(0);
    case 0xf6: RET(0); /* BM_GETIMAGE */
    case 0xf7: RET(0); /* BM_SETIMAGE */
    default:
        RET(rc_def_window_proc(c, hwnd, msg, wp, lp));
    }
}

/* ---- STATIC ---- */

typedef struct stat {
    uint32_t image;
} stat;

static void wp_static(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), msg = RC_ARG(c, 1), wp = RC_ARG(c, 2), lp = RC_ARG(c, 3);
    uint32_t style = rc_wnd_style(hwnd), t = style & 0x1f;
    switch (msg) {
    case WM_NCHITTEST:
        RET((style & 0x100) ? 1 : 0xffffffffu); /* SS_NOTIFY: HTCLIENT, else HTTRANSPARENT */
    case WM_ERASEBKGND:
        RET(1);
    case WM_PAINT: {
        paint p;
        if (!begin(c, hwnd, WM_CTLCOLORSTATIC, &p)) {
            RET(0);
        }
        rc_dc *d = p.d;
        rc_rect r = rc_wnd_client(hwnd);
        switch (t) {
        case 4: rc_dc_fill(d, r, rc_sys_color(21)); break; /* SS_BLACKRECT */
        case 5: rc_dc_fill(d, r, rc_sys_color(16)); break;
        case 6: rc_dc_fill(d, r, rc_sys_color(20)); break;
        case 7: case 8: case 9: {
            uint32_t k = rc_sys_color(t == 7 ? 21 : t == 8 ? 16 : 20);
            rc_dc_fill(d, (rc_rect){r.l, r.t, r.r, r.t + 1}, k);
            rc_dc_fill(d, (rc_rect){r.l, r.b - 1, r.r, r.b}, k);
            rc_dc_fill(d, (rc_rect){r.l, r.t, r.l + 1, r.b}, k);
            rc_dc_fill(d, (rc_rect){r.r - 1, r.t, r.r, r.b}, k);
            break;
        }
        case 0x10: rc_draw_edge(d, (rc_rect){r.l, r.t, r.r, r.t + 2}, 0x6, 0xf); break; /* ETCHEDHORZ */
        case 0x11: rc_draw_edge(d, (rc_rect){r.l, r.t, r.l + 2, r.b}, 0x6, 0xf); break;
        case 0x12: rc_draw_edge(d, r, 0x6, 0xf); break;
        case 0xd: draw_owner(c, hwnd, p.hdc, 3 /* ODT_STATIC */, 0, 1, 0, r, 0); break;
        case 3: case 0xe: { /* SS_ICON, SS_BITMAP */
            if (p.has_bg) {
                rc_dc_fill(d, r, p.bg);
            }
            stat *s = rc_wnd_data(hwnd);
            gobj *bm = s ? rc_gobj_get(s->image, OB_BITMAP) : NULL;
            if (bm && bm->pixels) {
                rc_dc src;
                memset(&src, 0, sizeof src);
                rc_gdi_bitmap_pixels(bm, &src.target);
                src.vis.n = -1;
                for (int32_t y = 0; y < (int32_t)bm->h && y < r.b; y++) {
                    for (int32_t x = 0; x < (int32_t)bm->w && x < r.r; x++) {
                        rc_dc_plot(d, x, y, rc_dc_pixel(d, rc_dc_colorref(&src, rc_dc_read(&src, x, y))));
                    }
                }
            }
            break;
        }
        default: {
            if (p.has_bg) {
                rc_dc_fill(d, r, p.bg);
            }
            d->bk_mode = 1;
            if (!rc_wnd_enabled(hwnd)) {
                d->text_color = rc_sys_color(17);
            }
            uint32_t fmt = (t == 1 ? 0x1 : t == 2 ? 0x2 : 0) | (t == 0xb || t == 0xc ? 0x20 : 0x10) |
                           ((style & 0x80) ? 0x800 : 0) | ((style & 0x200) ? 0x24 : 0) | ((style & 0xc000) ? 0x8000 : 0);
            rc_draw_text(d, rc_wnd_text(hwnd), -1, &r, fmt);
            break;
        }
        }
        if (style & 0x1000) { /* SS_SUNKEN */
            rc_draw_edge(d, r, 0x2, 0xf);
        }
        end(&p);
        RET(0);
    }
    case WM_SETTEXT:
    case WM_ENABLE: {
        uint32_t r = rc_def_window_proc(c, hwnd, msg, wp, lp);
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(r);
    }
    case 0x170:   /* STM_SETICON */
    case 0x172: { /* STM_SETIMAGE */
        stat *s = rc_wnd_data(hwnd);
        if (!s) {
            s = calloc(1, sizeof *s);
            rc_wnd_set_data(hwnd, s);
        }
        uint32_t old = s->image;
        s->image = msg == 0x170 ? wp : lp;
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(old);
    }
    case 0x171:
    case 0x173: {
        stat *s = rc_wnd_data(hwnd);
        RET(s ? s->image : 0);
    }
    default:
        RET(rc_def_window_proc(c, hwnd, msg, wp, lp));
    }
}

/* ---- EDIT ---- */

typedef struct edit {
    char *text;
    uint32_t len, cap, limit;
    uint32_t caret, anchor; /* selection: from anchor to caret */
    int32_t scroll_x;       /* single line: pixels scrolled */
    int32_t top_line;       /* multi line */
    bool modified;
    char password;
} edit;

static edit *ed(uint32_t hwnd)
{
    edit *e = rc_wnd_data(hwnd);
    if (!e) {
        e = calloc(1, sizeof *e);
        e->cap = 64;
        e->text = calloc(1, e->cap);
        e->limit = 30000;
        rc_wnd_set_data(hwnd, e);
    }
    return e;
}

static bool multiline(uint32_t hwnd) { return rc_wnd_style(hwnd) & 4; }

static void ed_set(edit *e, const char *t)
{
    uint32_t n = (uint32_t)strlen(t);
    if (n + 1 > e->cap) {
        e->cap = n + 64;
        e->text = realloc(e->text, e->cap);
    }
    memcpy(e->text, t, n + 1);
    e->len = n;
    e->caret = e->anchor = 0;
    e->scroll_x = 0;
    e->top_line = 0;
}

static void ed_replace(rc_cpu *c, uint32_t hwnd, edit *e, const char *ins, bool notify_change)
{
    uint32_t a = e->anchor < e->caret ? e->anchor : e->caret, b = e->anchor < e->caret ? e->caret : e->anchor;
    uint32_t n = (uint32_t)strlen(ins);
    uint32_t style = rc_wnd_style(hwnd);
    char buf[1024];
    uint32_t k = 0;
    for (uint32_t i = 0; i < n && k < sizeof buf - 1; i++) {
        char ch = ins[i];
        if ((style & 0x2000) && !(ch >= '0' && ch <= '9')) { /* ES_NUMBER */
            continue;
        }
        if (style & 0x8) {
            ch = (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
        } else if (style & 0x10) {
            ch = (char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
        }
        if (!multiline(hwnd) && (ch == '\r' || ch == '\n')) {
            continue;
        }
        buf[k++] = ch;
    }
    buf[k] = 0;
    if (e->len - (b - a) + k > e->limit) {
        k = e->limit > e->len - (b - a) ? e->limit - (e->len - (b - a)) : 0;
        if (notify_change) {
            notify(c, hwnd, 0x501 /* EN_MAXTEXT */);
        }
    }
    if (e->len - (b - a) + k + 1 > e->cap) {
        e->cap = e->len + k + 64;
        e->text = realloc(e->text, e->cap);
    }
    memmove(e->text + a + k, e->text + b, e->len - b + 1);
    memcpy(e->text + a, buf, k);
    e->len = e->len - (b - a) + k;
    e->caret = e->anchor = a + k;
    e->modified = true;
    rc_wnd_set_text(hwnd, e->text);
    rc_wnd_invalidate(hwnd, NULL, true);
    if (notify_change) {
        notify(c, hwnd, 0x400 /* EN_UPDATE */);
        notify(c, hwnd, 0x300 /* EN_CHANGE */);
    }
}

/* Lines of a multi-line edit: the start of line `n` (or the length), how many there are. */
static uint32_t line_start(edit *e, int32_t n)
{
    uint32_t at = 0;
    for (int32_t k = 0; k < n && at < e->len; at++) {
        if (e->text[at] == '\n') {
            k++;
        }
    }
    return n <= 0 ? 0 : at;
}

static int32_t line_of(edit *e, uint32_t pos)
{
    int32_t n = 0;
    for (uint32_t i = 0; i < pos && i < e->len; i++) {
        n += e->text[i] == '\n';
    }
    return n;
}

static uint32_t line_len(edit *e, uint32_t start)
{
    uint32_t i = start;
    while (i < e->len && e->text[i] != '\n' && e->text[i] != '\r') {
        i++;
    }
    return i - start;
}

static int32_t lines_of(edit *e) { return line_of(e, e->len) + 1; }

static int ed_width(rc_dc *d, const edit *e, uint32_t from, uint32_t n)
{
    int w = 0, h = 0;
    if (!n) {
        return 0;
    }
    if (e->password) {
        char tmp[1024];
        memset(tmp, e->password, n < 1023 ? n : 1023);
        rc_gdi_text_size(d, tmp, n < 1023 ? n : 1023, &w, &h);
    } else {
        rc_gdi_text_size(d, e->text + from, n, &w, &h);
    }
    return w;
}

static void edit_paint(rc_cpu *c, uint32_t hwnd)
{
    edit *e = ed(hwnd);
    bool ro = rc_wnd_style(hwnd) & 0x800;
    paint p;
    if (!begin(c, hwnd, ro ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT, &p)) {
        return;
    }
    rc_dc *d = p.d;
    rc_rect r = rc_wnd_client(hwnd);
    rc_dc_fill(d, r, rc_wnd_enabled(hwnd) ? p.bg : rc_sys_color(15));
    if (!rc_wnd_enabled(hwnd)) {
        d->text_color = rc_sys_color(17);
    }
    rc_font *f = rc_gdi_font_of(d);
    rc_font_metrics m = {13, 3, 16, 0, 7, 14};
    if (f) {
        rc_font_metrics_of(f, &m);
    }
    bool focus = rc_wnd_focus() == hwnd;
    uint32_t sa = e->anchor < e->caret ? e->anchor : e->caret, sb = e->anchor < e->caret ? e->caret : e->anchor;
    int32_t lines = multiline(hwnd) ? lines_of(e) : 1;
    for (int32_t ln = multiline(hwnd) ? e->top_line : 0; ln < lines; ln++) {
        int32_t y = 1 + (ln - (multiline(hwnd) ? e->top_line : 0)) * m.height;
        if (y > r.b) {
            break;
        }
        uint32_t start = multiline(hwnd) ? line_start(e, ln) : 0, n = multiline(hwnd) ? line_len(e, start) : e->len;
        int32_t x0 = 2 - (multiline(hwnd) ? 0 : e->scroll_x);
        uint32_t align = rc_wnd_style(hwnd) & 3;
        if (align && !multiline(hwnd)) {
            int w = ed_width(d, e, start, n);
            x0 = align == 1 ? (r.r - w) / 2 : r.r - w - 2;
        }
        /* text, with the selection drawn highlighted when focused */
        for (uint32_t i = 0; i < n;) {
            uint32_t at = start + i;
            bool sel = focus && at >= sa && at < sb;
            uint32_t j = i;
            while (j < n && (focus && start + j >= sa && start + j < sb) == sel) {
                j++;
            }
            int32_t x = x0 + ed_width(d, e, start, i), w = ed_width(d, e, at, j - i);
            uint32_t tc = d->text_color;
            if (sel) {
                rc_dc_fill(d, (rc_rect){x, y, x + w, y + m.height}, rc_sys_color(13));
                d->text_color = rc_sys_color(14);
            }
            if (e->password) {
                char tmp[1024];
                uint32_t k = j - i < 1023 ? j - i : 1023;
                memset(tmp, e->password, k);
                rc_gdi_text(d, x, y, tmp, k);
            } else {
                rc_gdi_text(d, x, y, e->text + at, j - i);
            }
            d->text_color = tc;
            i = j;
        }
        if (focus && sa == sb && line_of(e, e->caret) == ln) {
            int32_t cx = x0 + ed_width(d, e, start, e->caret - start);
            rc_dc_fill(d, (rc_rect){cx, y, cx + 1, y + m.height}, rc_sys_color(8));
        }
    }
    end(&p);
}

/* The character position at client point (x, y). */
static uint32_t ed_hit(rc_cpu *c, uint32_t hwnd, edit *e, int32_t x, int32_t y)
{
    uint32_t hdc = rc_wnd_dc(hwnd, false);
    rc_dc *d = rc_dc_get(hdc);
    uint32_t pos = 0;
    if (d) {
        uint32_t font = rc_def_window_proc(c, hwnd, WM_GETFONT, 0, 0);
        d->font = font ? font : rc_gui_font();
        rc_font *f = rc_gdi_font_of(d);
        rc_font_metrics m = {13, 3, 16, 0, 7, 14};
        if (f) {
            rc_font_metrics_of(f, &m);
        }
        int32_t ln = multiline(hwnd) ? e->top_line + (y - 1) / (m.height ? m.height : 16) : 0;
        if (ln >= lines_of(e)) {
            ln = lines_of(e) - 1;
        }
        uint32_t start = multiline(hwnd) ? line_start(e, ln) : 0, n = multiline(hwnd) ? line_len(e, start) : e->len;
        int32_t px = x - 2 + (multiline(hwnd) ? 0 : e->scroll_x);
        pos = start + n;
        for (uint32_t i = 0; i <= n; i++) {
            if (ed_width(d, e, start, i) + (i < n ? ed_width(d, e, start + i, 1) / 2 : 0) >= px) {
                pos = start + i;
                break;
            }
        }
    }
    rc_wnd_release_dc(hdc);
    return pos;
}

/* Keep the caret in view (single line: scroll sideways). */
static void ed_follow(rc_cpu *c, uint32_t hwnd, edit *e)
{
    if (multiline(hwnd)) {
        int32_t ln = line_of(e, e->caret);
        rc_rect r = rc_wnd_client(hwnd);
        int32_t visible = (r.b - 2) / 16;
        if (ln < e->top_line) {
            e->top_line = ln;
        } else if (visible > 0 && ln >= e->top_line + visible) {
            e->top_line = ln - visible + 1;
        }
        return;
    }
    uint32_t hdc = rc_wnd_dc(hwnd, false);
    rc_dc *d = rc_dc_get(hdc);
    if (d) {
        uint32_t font = rc_def_window_proc(c, hwnd, WM_GETFONT, 0, 0);
        d->font = font ? font : rc_gui_font();
        int32_t cx = ed_width(d, e, 0, e->caret), w = rc_wnd_client(hwnd).r - 4;
        if (cx - e->scroll_x > w) {
            e->scroll_x = cx - w;
        } else if (cx < e->scroll_x) {
            e->scroll_x = cx;
        }
    }
    rc_wnd_release_dc(hdc);
}

static void wp_edit(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), msg = RC_ARG(c, 1), wp = RC_ARG(c, 2), lp = RC_ARG(c, 3);
    edit *e = ed(hwnd);
    bool ro = rc_wnd_style(hwnd) & 0x800;
    switch (msg) {
    case WM_CREATE:
        ed_set(e, rc_wnd_text(hwnd));
        if (rc_wnd_style(hwnd) & 0x20) { /* ES_PASSWORD */
            e->password = '*';
        }
        RET(0);
    case WM_PAINT:
        edit_paint(c, hwnd);
        RET(0);
    case WM_ERASEBKGND:
        RET(1);
    case WM_GETDLGCODE: {
        uint32_t code = DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTARROWS;
        if (multiline(hwnd) && (rc_wnd_style(hwnd) & 0x1000)) { /* ES_WANTRETURN */
            code |= DLGC_WANTALLKEYS;
        }
        RET(code);
    }
    case WM_SETTEXT:
        ed_set(e, lp ? rc_str(lp) : "");
        rc_def_window_proc(c, hwnd, msg, wp, lp);
        rc_wnd_invalidate(hwnd, NULL, true);
        notify(c, hwnd, 0x300);
        RET(1);
    case WM_GETTEXT:
        RET(wp ? rc_put_str(lp, wp, e->text) : 0);
    case WM_GETTEXTLENGTH:
        RET(e->len);
    case WM_SETFOCUS:
        rc_wnd_invalidate(hwnd, NULL, true);
        notify(c, hwnd, 0x100 /* EN_SETFOCUS */);
        RET(0);
    case WM_KILLFOCUS:
        e->anchor = e->caret;
        rc_wnd_invalidate(hwnd, NULL, true);
        notify(c, hwnd, 0x200 /* EN_KILLFOCUS */);
        RET(0);
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        rc_wnd_set_focus(c, hwnd);
        uint32_t pos = ed_hit(c, hwnd, e, (int16_t)(lp & 0xffff), (int16_t)(lp >> 16));
        if (msg == WM_LBUTTONDBLCLK) {
            e->anchor = 0;
            e->caret = e->len;
        } else {
            e->caret = pos;
            if (!(rc_key_state(0x10) & 0x80)) {
                e->anchor = pos;
            }
            rc_wnd_set_capture(hwnd);
        }
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(0);
    }
    case WM_MOUSEMOVE:
        if (rc_wnd_capture() == hwnd && (wp & 1)) {
            e->caret = ed_hit(c, hwnd, e, (int16_t)(lp & 0xffff), (int16_t)(lp >> 16));
            ed_follow(c, hwnd, e);
            rc_wnd_invalidate(hwnd, NULL, true);
        }
        RET(0);
    case WM_LBUTTONUP:
        if (rc_wnd_capture() == hwnd) {
            rc_wnd_set_capture(0);
        }
        RET(0);
    case WM_CHAR: {
        if (ro || (rc_key_state(0x11) & 0x80)) {
            if (wp == 1) { /* Ctrl+A */
                e->anchor = 0;
                e->caret = e->len;
                rc_wnd_invalidate(hwnd, NULL, true);
            }
            RET(0);
        }
        if (wp == 8) { /* backspace */
            if (e->anchor == e->caret && e->caret > 0) {
                e->anchor = e->caret - 1;
            }
            ed_replace(c, hwnd, e, "", true);
        } else if (wp == '\r') {
            if (multiline(hwnd)) {
                ed_replace(c, hwnd, e, "\r\n", true);
            }
        } else if (wp >= 0x20 || wp == '\t') {
            if (wp == '\t' && !multiline(hwnd)) {
                RET(0);
            }
            char s[2] = {(char)wp, 0};
            ed_replace(c, hwnd, e, s, true);
        }
        ed_follow(c, hwnd, e);
        RET(0);
    }
    case WM_KEYDOWN: {
        bool shift = rc_key_state(0x10) & 0x80;
        uint32_t pos = e->caret;
        switch (wp) {
        case 0x25: pos = pos ? pos - 1 : 0; break;
        case 0x27: pos = pos < e->len ? pos + 1 : pos; break;
        case 0x24: pos = multiline(hwnd) ? line_start(e, line_of(e, pos)) : 0; break;
        case 0x23: pos = multiline(hwnd) ? line_start(e, line_of(e, pos)) + line_len(e, line_start(e, line_of(e, pos))) : e->len; break;
        case 0x26:
        case 0x28:
            if (multiline(hwnd)) {
                int32_t ln = line_of(e, pos) + (wp == 0x28 ? 1 : -1);
                if (ln >= 0 && ln < lines_of(e)) {
                    uint32_t col = pos - line_start(e, line_of(e, pos)), s = line_start(e, ln);
                    pos = s + (col < line_len(e, s) ? col : line_len(e, s));
                }
            }
            break;
        case 0x2e: /* Delete */
            if (!ro) {
                if (e->anchor == e->caret && e->caret < e->len) {
                    e->anchor = e->caret + 1;
                }
                ed_replace(c, hwnd, e, "", true);
            }
            RET(0);
        default:
            RET(0);
        }
        e->caret = pos;
        if (!shift) {
            e->anchor = pos;
        }
        ed_follow(c, hwnd, e);
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(0);
    }
    case WM_ENABLE:
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(0);
    case 0xb0: /* EM_GETSEL */
        if (wp) {
            rc_st32(wp, e->anchor < e->caret ? e->anchor : e->caret);
        }
        if (lp) {
            rc_st32(lp, e->anchor < e->caret ? e->caret : e->anchor);
        }
        RET(((e->anchor < e->caret ? e->caret : e->anchor) << 16) | (e->anchor < e->caret ? e->anchor : e->caret));
    case 0xb1: { /* EM_SETSEL */
        int32_t a = (int32_t)wp, b = (int32_t)lp;
        if (a < 0) {
            e->anchor = e->caret;
        } else {
            e->anchor = (uint32_t)a > e->len ? e->len : (uint32_t)a;
            e->caret = b < 0 || (uint32_t)b > e->len ? e->len : (uint32_t)b;
        }
        ed_follow(c, hwnd, e);
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(1);
    }
    case 0xb7: ed_follow(c, hwnd, e); RET(1); /* EM_SCROLLCARET */
    case 0xb8: RET(e->modified);             /* EM_GETMODIFY */
    case 0xb9: e->modified = wp != 0; RET(0);
    case 0xba: RET((uint32_t)lines_of(e));   /* EM_GETLINECOUNT */
    case 0xbb:                                          /* EM_LINEINDEX */
        RET(line_start(e, (int32_t)wp == -1 ? line_of(e, e->caret) : (int32_t)wp));
    case 0xc1: RET(line_len(e, line_start(e, line_of(e, wp)))); /* EM_LINELENGTH (by position) */
    case 0xc9: RET((uint32_t)line_of(e, wp));                  /* EM_LINEFROMCHAR */
    case 0xce: RET((uint32_t)e->top_line);                     /* EM_GETFIRSTVISIBLELINE */
    case 0xc4: { /* EM_GETLINE: the buffer's first word is its size */
        uint32_t s = line_start(e, (int32_t)wp), n = line_len(e, s), cap = rc_ld16(lp);
        if (n > cap) {
            n = cap;
        }
        memcpy(rc_ptr(lp), e->text + s, n);
        RET(n);
    }
    case 0xc2: /* EM_REPLACESEL */
        ed_replace(c, hwnd, e, lp ? rc_str(lp) : "", false);
        RET(0);
    case 0xc5: e->limit = wp ? wp : 0x7ffffffe; RET(0); /* EM_LIMITTEXT */
    case 0xd5: RET(e->limit);                         /* EM_GETLIMITTEXT */
    case 0xc6: RET(0);                                /* EM_CANUNDO */
    case 0xc7: RET(0);                                /* EM_UNDO */
    case 0xcc: e->password = (char)wp; rc_wnd_invalidate(hwnd, NULL, true); RET(0); /* EM_SETPASSWORDCHAR */
    case 0xcf: /* EM_SETREADONLY */
        rc_wnd_set_style(hwnd, wp ? rc_wnd_style(hwnd) | 0x800 : rc_wnd_style(hwnd) & ~0x800u);
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(1);
    case 0xd3: RET(0); /* EM_SETMARGINS */
    case 0x0300: /* WM_CUT */
    case 0x0302: /* WM_PASTE */
    case 0x0303: /* WM_CLEAR */
        if (msg != 0x302 && !ro) {
            ed_replace(c, hwnd, e, "", true);
        }
        RET(0);
    default:
        RET(rc_def_window_proc(c, hwnd, msg, wp, lp));
    }
}

/* ---- LISTBOX and the list of a COMBOBOX ---- */

typedef struct list {
    char **text;
    uint32_t *data;
    bool *sel;
    int32_t n, cap, cur, top, anchor, item_h;
} list;

static void list_free(list *l)
{
    for (int32_t i = 0; i < l->n; i++) {
        free(l->text[i]);
    }
    free(l->text);
    free(l->data);
    free(l->sel);
    memset(l, 0, sizeof *l);
    l->cur = -1;
}

static int ci_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : (unsigned char)*a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : (unsigned char)*b;
        if (x != y || !x) {
            return x - y;
        }
    }
}

static int32_t list_insert(list *l, int32_t at, const char *text, uint32_t data, bool sorted)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? 2 * l->cap : 16;
        l->text = realloc(l->text, sizeof(char *) * (size_t)l->cap);
        l->data = realloc(l->data, sizeof(uint32_t) * (size_t)l->cap);
        l->sel = realloc(l->sel, sizeof(bool) * (size_t)l->cap);
    }
    if (sorted) {
        at = 0;
        while (at < l->n && ci_cmp(l->text[at], text) <= 0) {
            at++;
        }
    } else if (at < 0 || at > l->n) {
        at = l->n;
    }
    memmove(&l->text[at + 1], &l->text[at], sizeof(char *) * (size_t)(l->n - at));
    memmove(&l->data[at + 1], &l->data[at], sizeof(uint32_t) * (size_t)(l->n - at));
    memmove(&l->sel[at + 1], &l->sel[at], sizeof(bool) * (size_t)(l->n - at));
    l->text[at] = strdup(text);
    l->data[at] = data;
    l->sel[at] = false;
    l->n++;
    if (l->cur >= at) {
        l->cur++;
    }
    return at;
}

static void list_delete(list *l, int32_t i)
{
    free(l->text[i]);
    memmove(&l->text[i], &l->text[i + 1], sizeof(char *) * (size_t)(l->n - i - 1));
    memmove(&l->data[i], &l->data[i + 1], sizeof(uint32_t) * (size_t)(l->n - i - 1));
    memmove(&l->sel[i], &l->sel[i + 1], sizeof(bool) * (size_t)(l->n - i - 1));
    l->n--;
    if (l->cur == i) {
        l->cur = -1;
    } else if (l->cur > i) {
        l->cur--;
    }
}

static int32_t list_find(list *l, int32_t after, const char *s, bool exact)
{
    size_t n = strlen(s);
    for (int32_t k = 0; k < l->n; k++) {
        int32_t i = (after + 1 + k) % l->n;
        if (exact ? !ci_cmp(l->text[i], s) : !strncmp(l->text[i], s, n) || ci_cmp(l->text[i], s) == 0) {
            return i;
        }
        if (!exact) {
            char tmp[256];
            snprintf(tmp, sizeof tmp, "%.*s", (int)n, l->text[i]);
            if (!ci_cmp(tmp, s)) {
                return i;
            }
        }
    }
    return -1;
}

typedef struct listbox {
    list l;
} listbox;

static list *lb(uint32_t hwnd)
{
    listbox *b = rc_wnd_data(hwnd);
    if (!b) {
        b = calloc(1, sizeof *b);
        b->l.cur = -1;
        b->l.item_h = 16;
        rc_wnd_set_data(hwnd, b);
    }
    return &b->l;
}

static int32_t lb_visible(uint32_t hwnd, list *l)
{
    int32_t h = rc_wnd_client(hwnd).b;
    return l->item_h > 0 ? (h / l->item_h > 0 ? h / l->item_h : 1) : 1;
}

static void lb_scrollbar(rc_cpu *c, uint32_t hwnd, list *l)
{
    if (!(rc_wnd_style(hwnd) & WS_VSCROLL) && l->n <= lb_visible(hwnd, l)) {
        return;
    }
    rc_scroll *s = rc_wnd_scroll(hwnd, 1);
    s->min = 0;
    s->max = l->n ? l->n - 1 : 0;
    s->page = lb_visible(hwnd, l);
    s->pos = l->top;
    (void)c;
}

static void lb_draw(rc_cpu *c, uint32_t hwnd, list *l, rc_dc *d, uint32_t hdc, rc_rect r, uint32_t bg, bool focus,
                    bool owner_draw)
{
    rc_dc_fill(d, r, bg);
    d->bk_mode = 1;
    for (int32_t i = l->top; i < l->n; i++) {
        int32_t y = r.t + (i - l->top) * l->item_h;
        if (y >= r.b) {
            break;
        }
        rc_rect ir = {r.l, y, r.r, y + l->item_h};
        bool sel = l->sel[i] || i == l->cur;
        if (owner_draw) {
            draw_owner(c, hwnd, hdc, 2 /* ODT_LISTBOX */, (uint32_t)i, 1, (sel ? 1 : 0) | (focus && i == l->cur ? 0x10 : 0), ir,
                       l->data[i]);
            continue;
        }
        uint32_t tc = d->text_color;
        if (sel) {
            rc_dc_fill(d, ir, rc_sys_color(13));
            d->text_color = rc_sys_color(14);
        }
        rc_rect tr = {ir.l + 2, ir.t, ir.r, ir.b};
        rc_draw_text(d, l->text[i], -1, &tr, 0x824 | 0x40);
        d->text_color = tc;
        if (focus && i == l->cur) {
            rc_draw_focus(d, ir);
        }
    }
}

static void lb_select(rc_cpu *c, uint32_t hwnd, list *l, int32_t i, bool user)
{
    uint32_t style = rc_wnd_style(hwnd);
    if (i < 0 || i >= l->n) {
        return;
    }
    if (style & 0x8) { /* LBS_MULTIPLESEL: toggles */
        l->sel[i] = !l->sel[i];
    } else if (style & 0x800) { /* LBS_EXTENDEDSEL */
        bool shift = rc_key_state(0x10) & 0x80, ctrl = rc_key_state(0x11) & 0x80;
        if (!ctrl) {
            memset(l->sel, 0, sizeof(bool) * (size_t)l->n);
        }
        if (shift && l->anchor >= 0) {
            for (int32_t k = (l->anchor < i ? l->anchor : i); k <= (l->anchor < i ? i : l->anchor); k++) {
                l->sel[k] = true;
            }
        } else {
            l->sel[i] = ctrl ? !l->sel[i] : true;
            l->anchor = i;
        }
    }
    l->cur = i;
    int32_t vis = lb_visible(hwnd, l);
    if (i < l->top) {
        l->top = i;
    } else if (i >= l->top + vis) {
        l->top = i - vis + 1;
    }
    lb_scrollbar(c, hwnd, l);
    rc_wnd_invalidate(hwnd, NULL, true);
    rc_wnd_redraw_frame(hwnd);
    if (user && (style & 1)) { /* LBS_NOTIFY */
        notify(c, hwnd, 1 /* LBN_SELCHANGE */);
    }
}

/* The messages a list box and a combo box share (LB_* and CB_* map onto each other). */
static bool list_message(rc_cpu *c, uint32_t hwnd, list *l, uint32_t m, uint32_t wp, uint32_t lp, bool sorted,
                         bool has_strings, uint32_t *r)
{
    switch (m) {
    case 0: /* ADDSTRING */
        *r = (uint32_t)list_insert(l, -1, has_strings ? rc_str(lp) : "", has_strings ? 0 : lp, sorted);
        return true;
    case 1: /* INSERTSTRING */
        *r = (uint32_t)list_insert(l, (int32_t)wp, has_strings ? rc_str(lp) : "", has_strings ? 0 : lp, false);
        return true;
    case 2: /* DELETESTRING */
        if ((int32_t)wp < 0 || (int32_t)wp >= l->n) {
            *r = 0xffffffffu;
        } else {
            list_delete(l, (int32_t)wp);
            *r = (uint32_t)l->n;
        }
        return true;
    case 3: /* RESETCONTENT */
        {
            int32_t ih = l->item_h;
            list_free(l);
            l->item_h = ih;
        }
        *r = 0;
        return true;
    case 4: /* GETCOUNT */
        *r = (uint32_t)l->n;
        return true;
    case 5: /* GETCURSEL */
        *r = (uint32_t)l->cur;
        return true;
    case 6: /* SETCURSEL */
        l->cur = (int32_t)wp < l->n ? (int32_t)wp : -1;
        if (l->n) {
            memset(l->sel, 0, sizeof(bool) * (size_t)l->n);
        }
        *r = (uint32_t)l->cur;
        return true;
    case 7: /* GETTEXT */
        if ((int32_t)wp < 0 || (int32_t)wp >= l->n) {
            *r = 0xffffffffu;
        } else if (!has_strings) {
            rc_st32(lp, l->data[wp]);
            *r = 4;
        } else {
            *r = rc_put_str(lp, 0x7fffffff, l->text[wp]);
        }
        return true;
    case 8: /* GETTEXTLEN */
        *r = (int32_t)wp < 0 || (int32_t)wp >= l->n ? 0xffffffffu : (uint32_t)strlen(l->text[wp]);
        return true;
    case 9: /* GETITEMDATA */
        *r = (int32_t)wp < 0 || (int32_t)wp >= l->n ? 0xffffffffu : l->data[wp];
        return true;
    case 10: /* SETITEMDATA */
        if ((int32_t)wp >= 0 && (int32_t)wp < l->n) {
            l->data[wp] = lp;
            *r = 1;
        } else {
            *r = 0xffffffffu;
        }
        return true;
    case 11: /* FINDSTRING */
        *r = (uint32_t)(l->n ? list_find(l, (int32_t)wp, rc_str(lp), false) : -1);
        return true;
    case 12: /* FINDSTRINGEXACT */
        *r = (uint32_t)(l->n ? list_find(l, (int32_t)wp, rc_str(lp), true) : -1);
        return true;
    case 13: { /* SELECTSTRING */
        int32_t i = l->n ? list_find(l, (int32_t)wp, rc_str(lp), false) : -1;
        if (i >= 0) {
            l->cur = i;
        }
        *r = (uint32_t)i;
        return true;
    }
    case 14: /* SETITEMHEIGHT */
        l->item_h = (int32_t)(lp & 0xffff);
        *r = 0;
        return true;
    case 15: /* GETITEMHEIGHT */
        *r = (uint32_t)l->item_h;
        return true;
    default:
        (void)c;
        (void)hwnd;
        return false;
    }
}

static int lb_map(uint32_t msg)
{
    switch (msg) {
    case 0x180: return 0;
    case 0x181: return 1;
    case 0x182: return 2;
    case 0x184: return 3;
    case 0x18b: return 4;
    case 0x188: return 5;
    case 0x186: return 6;
    case 0x189: return 7;
    case 0x18a: return 8;
    case 0x199: return 9;
    case 0x19a: return 10;
    case 0x18f: return 11;
    case 0x1a2: return 12;
    case 0x18c: return 13;
    case 0x1a0: return 14;
    case 0x1a1: return 15;
    default: return -1;
    }
}

static int32_t font_height(rc_cpu *c, uint32_t hwnd)
{
    uint32_t hdc = rc_wnd_dc(hwnd, false);
    rc_dc *d = rc_dc_get(hdc);
    int32_t h = 16;
    if (d) {
        uint32_t font = rc_def_window_proc(c, hwnd, WM_GETFONT, 0, 0);
        d->font = font ? font : rc_gui_font();
        rc_font *f = rc_gdi_font_of(d);
        rc_font_metrics m;
        if (f) {
            rc_font_metrics_of(f, &m);
            h = m.height + 1;
        }
    }
    rc_wnd_release_dc(hdc);
    return h;
}

static void wp_listbox(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), msg = RC_ARG(c, 1), wp = RC_ARG(c, 2), lp = RC_ARG(c, 3);
    list *l = lb(hwnd);
    uint32_t style = rc_wnd_style(hwnd), r;
    bool owner = style & 0x30, has_strings = !owner || (style & 0x40);
    int k = lb_map(msg);
    if (k >= 0 && list_message(c, hwnd, l, (uint32_t)k, wp, lp, style & 2, has_strings, &r)) {
        lb_scrollbar(c, hwnd, l);
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(r);
    }
    switch (msg) {
    case WM_CREATE: {
        l->item_h = font_height(c, hwnd);
        if (owner) {
            uint32_t mis = rc_heap_alloc(24, true);
            rc_st32(mis, 2);
            rc_st32(mis + 4, rc_wnd_id(hwnd));
            rc_st32(mis + 12, (uint32_t)rc_wnd_client(hwnd).r);
            rc_st32(mis + 16, (uint32_t)l->item_h);
            rc_wnd_send(c, rc_wnd_parent(hwnd), WM_MEASUREITEM, rc_wnd_id(hwnd), mis);
            l->item_h = (int32_t)rc_ld32(mis + 16);
            rc_heap_free(mis);
        }
        RET(0);
    }
    case WM_PAINT: {
        paint p;
        if (begin(c, hwnd, WM_CTLCOLORLISTBOX, &p)) {
            lb_draw(c, hwnd, l, p.d, p.hdc, rc_wnd_client(hwnd), p.bg, rc_wnd_focus() == hwnd, owner);
            end(&p);
        }
        RET(0);
    }
    case WM_ERASEBKGND:
        RET(1);
    case WM_GETDLGCODE:
        RET(DLGC_WANTARROWS | DLGC_WANTCHARS);
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        rc_wnd_invalidate(hwnd, NULL, true);
        if (style & 1) {
            notify(c, hwnd, msg == WM_SETFOCUS ? 4 : 5);
        }
        RET(0);
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        rc_wnd_set_focus(c, hwnd);
        int32_t i = l->top + (int16_t)(lp >> 16) / (l->item_h ? l->item_h : 16);
        if (i < l->n) {
            lb_select(c, hwnd, l, i, true);
            if (msg == WM_LBUTTONDBLCLK && (style & 1)) {
                notify(c, hwnd, 2 /* LBN_DBLCLK */);
            }
        }
        RET(0);
    }
    case WM_KEYDOWN: {
        int32_t i = l->cur < 0 ? 0 : l->cur, vis = lb_visible(hwnd, l);
        switch (wp) {
        case 0x26: i--; break;
        case 0x28: i++; break;
        case 0x21: i -= vis; break;
        case 0x22: i += vis; break;
        case 0x24: i = 0; break;
        case 0x23: i = l->n - 1; break;
        default: RET(0);
        }
        i = i < 0 ? 0 : i >= l->n ? l->n - 1 : i;
        if (l->n) {
            if (!(style & 0x808)) {
                lb_select(c, hwnd, l, i, true);
            } else {
                l->cur = i;
                rc_wnd_invalidate(hwnd, NULL, true);
            }
        }
        RET(0);
    }
    case WM_MOUSEWHEEL:
    case WM_VSCROLL: {
        int32_t vis = lb_visible(hwnd, l), top = l->top;
        if (msg == WM_MOUSEWHEEL) {
            top -= (int16_t)(wp >> 16) / 40;
        } else {
            switch (wp & 0xffff) {
            case 0: top--; break;
            case 1: top++; break;
            case 2: top -= vis; break;
            case 3: top += vis; break;
            case 4: case 5: top = (int16_t)(wp >> 16); break;
            default: break;
            }
        }
        int32_t most = l->n - vis > 0 ? l->n - vis : 0;
        l->top = top < 0 ? 0 : top > most ? most : top;
        lb_scrollbar(c, hwnd, l);
        rc_wnd_invalidate(hwnd, NULL, true);
        rc_wnd_redraw_frame(hwnd); /* the frame's bar */
        RET(0);
    }
    case 0x185: /* LB_SETSEL(select, index): -1 all */
        if ((int32_t)lp == -1) {
            for (int32_t i = 0; i < l->n; i++) {
                l->sel[i] = wp != 0;
            }
        } else if ((int32_t)lp < l->n) {
            l->sel[lp] = wp != 0;
        }
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(0);
    case 0x187: /* LB_GETSEL */
        RET((int32_t)wp >= 0 && (int32_t)wp < l->n ? (l->sel[wp] || ((int32_t)wp == l->cur && !(style & 0x808))) : 0xffffffffu);
    case 0x190: { /* LB_GETSELCOUNT */
        uint32_t n = 0;
        for (int32_t i = 0; i < l->n; i++) {
            n += l->sel[i];
        }
        RET(n);
    }
    case 0x191: { /* LB_GETSELITEMS(max, array) */
        uint32_t n = 0;
        for (int32_t i = 0; i < l->n && n < wp; i++) {
            if (l->sel[i]) {
                rc_st32(lp + 4 * n++, (uint32_t)i);
            }
        }
        RET(n);
    }
    case 0x18e: RET((uint32_t)l->top); /* LB_GETTOPINDEX */
    case 0x197:                                  /* LB_SETTOPINDEX */
        l->top = (int32_t)wp < l->n ? (int32_t)wp : l->top;
        lb_scrollbar(c, hwnd, l);
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(0);
    case 0x198: { /* LB_GETITEMRECT */
        int32_t y = ((int32_t)wp - l->top) * l->item_h;
        rc_st32(lp, 0);
        rc_st32(lp + 4, (uint32_t)y);
        rc_st32(lp + 8, (uint32_t)rc_wnd_client(hwnd).r);
        rc_st32(lp + 12, (uint32_t)(y + l->item_h));
        RET(1);
    }
    case 0x19e: l->cur = (int32_t)wp; rc_wnd_invalidate(hwnd, NULL, true); RET(0); /* LB_SETCARETINDEX */
    case 0x19f: RET((uint32_t)l->cur);                                             /* LB_GETCARETINDEX */
    case 0x194: case 0x195: case 0x196: RET(0);                                    /* extents, tab stops */
    case 0x1a8: RET(0);                                                             /* LB_ITEMFROMPOINT */
    case 0x18d: RET(0xffffffffu);                                                   /* LB_DIR */
    case WM_DESTROY:
        list_free(l);
        RET(0);
    default:
        RET(rc_def_window_proc(c, hwnd, msg, wp, lp));
    }
}

/* ---- COMBOBOX ---- */

typedef struct combo {
    list l;
    char text[256];
    int32_t list_h;
    bool dropped;
} combo;

static combo *cb(uint32_t hwnd)
{
    combo *b = rc_wnd_data(hwnd);
    if (!b) {
        b = calloc(1, sizeof *b);
        b->l.cur = -1;
        b->l.item_h = 16;
        rc_wnd_set_data(hwnd, b);
    }
    return b;
}

static int cb_map(uint32_t msg)
{
    switch (msg) {
    case 0x143: return 0;
    case 0x14a: return 1;
    case 0x144: return 2;
    case 0x14b: return 3;
    case 0x146: return 4;
    case 0x147: return 5;
    case 0x14e: return 6;
    case 0x148: return 7;
    case 0x149: return 8;
    case 0x150: return 9;
    case 0x151: return 10;
    case 0x14c: return 11;
    case 0x158: return 12;
    case 0x14d: return 13;
    case 0x153: return 14;
    case 0x154: return 15;
    default: return -1;
    }
}

static void cb_sync(combo *b)
{
    if (b->l.cur >= 0 && b->l.cur < b->l.n) {
        snprintf(b->text, sizeof b->text, "%s", b->l.text[b->l.cur]);
    } else if (b->l.cur < 0) {
        b->text[0] = 0;
    }
}

/* The drop-down list: drawn on the desktop below the box while it is tracked. */
static void cb_drop(rc_cpu *c, uint32_t hwnd, combo *b)
{
    if (!b->l.n || !rc_desktop_active()) {
        return;
    }
    notify(c, hwnd, 7 /* CBN_DROPDOWN */);
    rc_rect wr = rc_wnd_screen_rect(hwnd);
    int32_t ih = b->l.item_h, rows = b->l.n < 12 ? b->l.n : 12;
    int32_t dw, dh;
    rc_desktop_size(&dw, &dh);
    rc_rect r = {wr.l, wr.b, wr.r, wr.b + rows * ih + 2};
    if (r.b > dh) {
        r = (rc_rect){wr.l, wr.t - rows * ih - 2, wr.r, wr.t};
    }
    uint32_t hdc = rc_wnd_dc(0, true);
    rc_dc *d = rc_dc_get(hdc);
    if (!d) {
        return;
    }
    uint32_t font = rc_def_window_proc(c, hwnd, WM_GETFONT, 0, 0);
    d->font = font ? font : rc_gui_font();
    int32_t w = r.r - r.l, h = r.b - r.t;
    uint32_t *saved = malloc(sizeof(uint32_t) * (size_t)(w * h));
    for (int32_t y = 0; saved && y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            saved[y * w + x] = rc_dc_read(d, r.l + x, r.t + y);
        }
    }
    list view = b->l;
    int32_t hot = b->l.cur, top = hot >= rows ? hot - rows + 1 : 0;
    b->dropped = true;
    bool chosen = false, done = false;
    while (!done) {
        view.cur = hot;
        view.top = top;
        rc_dc_fill(d, r, 0);
        lb_draw(c, hwnd, &view, d, hdc, (rc_rect){r.l + 1, r.t + 1, r.r - 1, r.b - 1}, rc_sys_color(5), false, false);
        rc_desktop_present(true);
        rc_event e;
        while (!rc_input_next_raw(&e)) {
            rc_thread_sleep(1);
        }
        if (e.type == RC_EV_QUIT) {
            rc_input_unread_raw(&e);
            break;
        }
        bool inside = e.x >= r.l && e.x < r.r && e.y > r.t && e.y < r.b - 1;
        int32_t at = top + (e.y - r.t - 1) / ih;
        if (e.type == RC_EV_MOUSE_MOVE && inside && at < b->l.n) {
            hot = at;
        } else if (e.type == RC_EV_MOUSE_BUTTON && !e.down && inside && at < b->l.n) {
            hot = at;
            chosen = done = true;
        } else if (e.type == RC_EV_MOUSE_BUTTON && e.down && !inside) {
            bool on_box = e.x >= wr.l && e.x < wr.r && e.y >= wr.t && e.y < wr.b;
            done = !on_box || true;
        } else if (e.type == RC_EV_WHEEL) {
            top -= e.delta / 40;
        } else if (e.type == RC_EV_KEY && e.down) {
            if (e.vk == 0x26 && hot > 0) {
                hot--;
            } else if (e.vk == 0x28 && hot + 1 < b->l.n) {
                hot++;
            } else if (e.vk == 0x0d) {
                chosen = done = true;
            } else if (e.vk == 0x1b) {
                done = true;
            }
        }
        if (hot >= 0 && hot < top) {
            top = hot;
        } else if (hot >= top + rows) {
            top = hot - rows + 1;
        }
        top = top < 0 ? 0 : top > b->l.n - rows ? b->l.n - rows : top;
    }
    for (int32_t y = 0; saved && y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            rc_dc_write(d, r.l + x, r.t + y, saved[y * w + x]);
        }
    }
    free(saved);
    rc_wnd_release_dc(hdc);
    b->dropped = false;
    rc_desktop_present(true);
    notify(c, hwnd, 8 /* CBN_CLOSEUP */);
    if (chosen && hot >= 0 && hot != b->l.cur) {
        b->l.cur = hot;
        cb_sync(b);
        rc_wnd_set_text(hwnd, b->text);
        rc_wnd_invalidate(hwnd, NULL, true);
        notify(c, hwnd, 9 /* CBN_SELENDOK */);
        notify(c, hwnd, 1 /* CBN_SELCHANGE */);
    } else {
        notify(c, hwnd, 10 /* CBN_SELENDCANCEL */);
    }
}

static void wp_combobox(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), msg = RC_ARG(c, 1), wp = RC_ARG(c, 2), lp = RC_ARG(c, 3);
    combo *b = cb(hwnd);
    uint32_t style = rc_wnd_style(hwnd), r;
    bool owner = style & 0x30, has_strings = !owner || (style & 0x200);
    int k = cb_map(msg);
    if (k >= 0 && list_message(c, hwnd, &b->l, (uint32_t)k, wp, lp, style & 0x100, has_strings, &r)) {
        if (k == 6 || k == 13 || k == 3) {
            cb_sync(b);
            rc_wnd_set_text(hwnd, b->text);
        }
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(r);
    }
    switch (msg) {
    case 0x0081: /* WM_NCCREATE: WS_VSCROLL and WS_HSCROLL are the list's, not the box's own */
        rc_wnd_set_style(hwnd, style & ~(WS_VSCROLL | WS_HSCROLL));
        RET(rc_def_window_proc(c, hwnd, msg, wp, lp));
    case WM_CREATE: {
        b->l.item_h = font_height(c, hwnd);
        rc_rect cr = rc_wnd_client(hwnd);
        b->list_h = cr.b;
        rc_rect wr = rc_wnd_screen_rect(hwnd);
        int32_t px = 0, py = 0;
        if (rc_wnd_parent(hwnd)) {
            rc_wnd_client_origin(rc_wnd_parent(hwnd), &px, &py);
        }
        int32_t field = b->l.item_h + 6;
        if ((style & 3) != 1 && wr.b - wr.t > field) { /* not CBS_SIMPLE: the box is one line high */
            rc_wnd_move(c, hwnd, wr.l - px, wr.t - py, wr.r - wr.l, field, false);
        }
        RET(0);
    }
    case WM_PAINT: {
        paint p;
        if (!begin(c, hwnd, (style & 3) == 3 ? WM_CTLCOLORLISTBOX : WM_CTLCOLOREDIT, &p)) {
            RET(0);
        }
        rc_dc *d = p.d;
        rc_rect cr = rc_wnd_client(hwnd);
        bool enabled = rc_wnd_enabled(hwnd), focus = rc_wnd_focus() == hwnd;
        rc_dc_fill(d, cr, enabled ? rc_sys_color(5) : rc_sys_color(15));
        rc_draw_edge(d, cr, 0xa, 0xf);
        rc_rect btnr = {cr.r - 18, cr.t + 2, cr.r - 2, cr.b - 2};
        rc_dc_fill(d, btnr, rc_sys_color(15));
        rc_draw_edge(d, btnr, 0x5, 0xf);
        int32_t ax = (btnr.l + btnr.r) / 2, ay = (btnr.t + btnr.b) / 2 - 1;
        for (int i = 0; i < 4; i++) {
            rc_dc_fill(d, (rc_rect){ax - 3 + i, ay + i, ax + 4 - i, ay + i + 1}, rc_sys_color(enabled ? 18 : 17));
        }
        rc_rect tr = {cr.l + 4, cr.t + 2, btnr.l - 2, cr.b - 2};
        d->bk_mode = 1;
        if (owner && b->l.cur >= 0) {
            draw_owner(c, hwnd, p.hdc, 3 /* ODT_COMBOBOX */, (uint32_t)b->l.cur, 1, focus ? 1 : 0, tr, b->l.data[b->l.cur]);
        } else {
            if (focus && (style & 3) == 3) {
                rc_dc_fill(d, tr, rc_sys_color(13));
                d->text_color = rc_sys_color(14);
            } else if (!enabled) {
                d->text_color = rc_sys_color(17);
            }
            rc_draw_text(d, b->text, -1, &tr, 0x824);
        }
        end(&p);
        RET(0);
    }
    case WM_ERASEBKGND:
        RET(1);
    case WM_GETDLGCODE:
        RET(DLGC_WANTARROWS | DLGC_WANTCHARS);
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        rc_wnd_invalidate(hwnd, NULL, true);
        notify(c, hwnd, msg == WM_SETFOCUS ? 3 : 4);
        RET(0);
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        rc_wnd_set_focus(c, hwnd);
        cb_drop(c, hwnd, b);
        RET(0);
    case WM_KEYDOWN: {
        int32_t i = b->l.cur;
        if (wp == 0x26 && i > 0) {
            i--;
        } else if (wp == 0x28 && i + 1 < b->l.n) {
            i++;
        } else if (wp == 0x73) { /* F4 */
            cb_drop(c, hwnd, b);
            RET(0);
        } else {
            RET(0);
        }
        b->l.cur = i;
        cb_sync(b);
        rc_wnd_set_text(hwnd, b->text);
        rc_wnd_invalidate(hwnd, NULL, true);
        notify(c, hwnd, 1);
        RET(0);
    }
    case WM_CHAR:
        if ((style & 3) != 3 && wp >= 0x20) { /* an editable box: type into it */
            size_t n = strlen(b->text);
            if (n + 1 < sizeof b->text) {
                b->text[n] = (char)wp;
                b->text[n + 1] = 0;
            }
            rc_wnd_set_text(hwnd, b->text);
            rc_wnd_invalidate(hwnd, NULL, true);
            notify(c, hwnd, 5 /* CBN_EDITCHANGE */);
        } else if ((style & 3) != 3 && wp == 8) {
            size_t n = strlen(b->text);
            if (n) {
                b->text[n - 1] = 0;
            }
            rc_wnd_set_text(hwnd, b->text);
            rc_wnd_invalidate(hwnd, NULL, true);
            notify(c, hwnd, 5);
        }
        RET(0);
    case WM_SETTEXT:
        snprintf(b->text, sizeof b->text, "%s", lp ? rc_str(lp) : "");
        rc_def_window_proc(c, hwnd, msg, wp, lp);
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(1);
    case WM_GETTEXT:
        RET(wp ? rc_put_str(lp, wp, b->text) : 0);
    case WM_GETTEXTLENGTH:
        RET((uint32_t)strlen(b->text));
    case WM_ENABLE:
        rc_wnd_invalidate(hwnd, NULL, true);
        RET(0);
    case 0x14f: /* CB_SHOWDROPDOWN */
        if (wp && !b->dropped) {
            cb_drop(c, hwnd, b);
        }
        RET(1);
    case 0x157: RET(b->dropped); /* CB_GETDROPPEDSTATE */
    case 0x152: {                          /* CB_GETDROPPEDCONTROLRECT */
        rc_rect wr = rc_wnd_screen_rect(hwnd);
        rc_st32(lp, (uint32_t)wr.l);
        rc_st32(lp + 4, (uint32_t)wr.t);
        rc_st32(lp + 8, (uint32_t)wr.r);
        rc_st32(lp + 12, (uint32_t)(wr.b + b->list_h));
        RET(1);
    }
    case 0x140: RET(0);       /* CB_GETEDITSEL */
    case 0x141: case 0x142: RET(1); /* CB_LIMITTEXT, CB_SETEDITSEL */
    case 0x155: case 0x156: RET(0); /* extended UI */
    case 0x15f: RET((uint32_t)rc_wnd_client(hwnd).r); /* CB_GETDROPPEDWIDTH */
    case 0x160: RET(wp);
    case 0x145: RET(0xffffffffu); /* CB_DIR */
    case WM_DESTROY:
        list_free(&b->l);
        RET(0);
    default:
        RET(rc_def_window_proc(c, hwnd, msg, wp, lp));
    }
}

static const rc_import_def defs[] = {
    {U32, "SetScrollInfo", k_SetScrollInfo},
    {U32, "GetScrollInfo", k_GetScrollInfo},
    {U32, "SetScrollPos", k_SetScrollPos},
    {U32, "GetScrollPos", k_GetScrollPos},
    {U32, "SetScrollRange", k_SetScrollRange},
    {U32, "GetScrollRange", k_GetScrollRange},
    {U32, "ShowScrollBar", k_ShowScrollBar},
    {U32, "EnableScrollBar", k_EnableScrollBar},
};

void rc_controls_init(void)
{
    rc_register_imports(defs, sizeof defs / sizeof defs[0]);
    uint32_t arrow = rc_arrow_cursor();
    rc_class_builtin("Button", 0x0b /* DBLCLKS VREDRAW HREDRAW */, wp_button, 8, 16, arrow);
    rc_class_builtin("Static", 0x0, wp_static, 8, 0, arrow);
    rc_class_builtin("Edit", 0x8, wp_edit, 8, 0, 0);
    rc_class_builtin("ListBox", 0x8, wp_listbox, 8, 0, arrow);
    rc_class_builtin("ComboBox", 0x8, wp_combobox, 8, 0, arrow);
    rc_class_builtin("ComboLBox", 0x8, wp_listbox, 8, 0, arrow);
    rc_class_builtin("ScrollBar", 0x8, wp_scrollbar, 8, 0, arrow);
}
