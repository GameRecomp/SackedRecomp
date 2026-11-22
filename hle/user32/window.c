/* SPDX-License-Identifier: MIT */
/*
 * user32: windows. A table of windows in a tree (children are placed in their parent's client
 * area), their frames (border, caption, menu bar, scroll bars around the client area), z-order,
 * text, painting (an update rectangle per window, painted parents first), device contexts, and
 * DefWindowProc with the non-client area.
 *
 * Two ways a program is shown:
 *  - A game shows itself through DirectDraw: its window has no frame (the display mode makes it
 *    cover the screen), window DCs draw nowhere, and input goes to the main window (input.c).
 *  - Any other program draws its windows with GDI onto our desktop: 32-bit pixels in guest
 *    memory, as large as RC_DESKTOP=WxH (default 1024x768), which the platform shows.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdi32/dc.h"
#include "gdi32/gdi32.h"
#include "gdi32/objects.h"
#include "rc/hle.h"
#include "rc/hle_modules.h"
#include "rc/log.h"
#include "rc/platform.h"
#include "rc/process.h"
#include "rc/thread.h"
#include "rc/vm.h"
#include "ddraw/ddraw.h"
#include "user32.h"

#define U32 "USER32.dll"

#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_MOVE 0x0003u
#define WM_SIZE 0x0005u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_KILLFOCUS 0x0008u
#define WM_ENABLE 0x000au
#define WM_SETREDRAW 0x000bu
#define WM_SETTEXT 0x000cu
#define WM_GETTEXT 0x000du
#define WM_GETTEXTLENGTH 0x000eu
#define WM_PAINT 0x000fu
#define WM_CLOSE 0x0010u
#define WM_QUIT 0x0012u
#define WM_ERASEBKGND 0x0014u
#define WM_SHOWWINDOW 0x0018u
#define WM_ACTIVATEAPP 0x001cu
#define WM_SETCURSOR 0x0020u
#define WM_MOUSEACTIVATE 0x0021u
#define WM_CHILDACTIVATE 0x0022u
#define WM_GETMINMAXINFO 0x0024u
#define WM_SETFONT 0x0030u
#define WM_GETFONT 0x0031u
#define WM_WINDOWPOSCHANGING 0x0046u
#define WM_WINDOWPOSCHANGED 0x0047u
#define WM_CONTEXTMENU 0x007bu
#define WM_STYLECHANGED 0x007du
#define WM_NCCREATE 0x0081u
#define WM_NCDESTROY 0x0082u
#define WM_NCCALCSIZE 0x0083u
#define WM_NCHITTEST 0x0084u
#define WM_NCPAINT 0x0085u
#define WM_NCACTIVATE 0x0086u
#define WM_NCLBUTTONDOWN 0x00a1u
#define WM_SYSKEYDOWN 0x0104u
#define WM_SYSKEYUP 0x0105u
#define WM_SYSCHAR 0x0106u
#define WM_SYSCOMMAND 0x0112u
#define WM_CTLCOLORMSGBOX 0x0132u
#define WM_CTLCOLORSTATIC 0x0138u
#define WM_RBUTTONUP 0x0205u
#define WM_PARENTNOTIFY 0x0210u

#define CW_USEDEFAULT 0x80000000u
#define HTNOWHERE 0u
#define HTCLIENT 1u
#define HTCAPTION 2u
#define HTMENU 5u
#define HTHSCROLL 6u
#define HTVSCROLL 7u
#define HTBORDER 18u
#define HTCLOSE 20u

/* ---- windows ---- */

#define WINDOWS 4096
#define HWND_BASE 0x00010010u
#define EXTRA 256
#define PROPS 8

typedef struct window {
    bool used, game;
    uint32_t thread; /* the thread that made it: its messages are handled there */
    uint32_t wndproc, style, exstyle, parent, owner, menu, id, instance, userdata;
    rc_wclass *cls;
    rc_rect rect;   /* in the parent's client coordinates (top-level: the screen) */
    rc_rect crect;  /* the client area, relative to the window's top-left */
    int64_t z;      /* larger: above its siblings */
    char *text;
    rc_rect upd;    /* what needs painting, client coordinates */
    bool erase, nc_dirty, redraw_off;
    void *data;
    uint32_t font;
    struct {
        uint32_t atom;
        char name[40];
        uint32_t value;
    } props[PROPS];
    rc_scroll sb[2];
    uint8_t extra[EXTRA];
} window;

static window windows[WINDOWS];
static uint32_t main_hwnd, focus_hwnd, capture_hwnd, active_hwnd;
static int64_t z_top = 1, z_bottom = 0;
/* Changes with every move, resize, show, hide, restacking, creation and destruction: window DCs
 * a program keeps recompute their origin and visible region when it has changed. */
static uint32_t vis_gen = 1;

static window *get(uint32_t hwnd)
{
    if (hwnd < HWND_BASE || (hwnd - HWND_BASE) % 4) {
        return NULL;
    }
    uint32_t i = (hwnd - HWND_BASE) / 4;
    return i < WINDOWS && windows[i].used ? &windows[i] : NULL;
}

static uint32_t handle_of(const window *w) { return HWND_BASE + 4 * (uint32_t)(w - windows); }

bool rc_wnd_ok(uint32_t hwnd) { return get(hwnd) != NULL; }
uint32_t rc_window_proc(uint32_t hwnd)
{
    window *w = get(hwnd);
    return w ? w->wndproc : 0;
}
uint32_t rc_main_window(void) { return main_hwnd; }
uint32_t rc_wnd_style(uint32_t hwnd)
{
    window *w = get(hwnd);
    return w ? w->style : 0;
}
void rc_wnd_set_style(uint32_t hwnd, uint32_t style)
{
    window *w = get(hwnd);
    if (w) {
        w->style = style;
    }
}
uint32_t rc_wnd_exstyle(uint32_t hwnd)
{
    window *w = get(hwnd);
    return w ? w->exstyle : 0;
}
uint32_t rc_wnd_parent(uint32_t hwnd)
{
    window *w = get(hwnd);
    return w && (w->style & WS_CHILD) ? w->parent : 0;
}
uint32_t rc_wnd_owner(uint32_t hwnd)
{
    window *w = get(hwnd);
    return w && !(w->style & WS_CHILD) ? w->owner : 0;
}
uint32_t rc_wnd_id(uint32_t hwnd)
{
    window *w = get(hwnd);
    return w ? w->id : 0;
}
uint32_t rc_wnd_instance(uint32_t hwnd)
{
    window *w = get(hwnd);
    return w ? w->instance : 0;
}
uint32_t rc_wnd_menu(uint32_t hwnd)
{
    window *w = get(hwnd);
    return w && !(w->style & WS_CHILD) ? w->menu : 0;
}
const char *rc_wnd_text(uint32_t hwnd)
{
    window *w = get(hwnd);
    return w && w->text ? w->text : "";
}
void rc_wnd_set_text(uint32_t hwnd, const char *text)
{
    window *w = get(hwnd);
    if (!w) {
        return;
    }
    char *copy = strdup(text ? text : "");
    free(w->text);
    w->text = copy;
}
const char *rc_wnd_class_name(uint32_t hwnd)
{
    window *w = get(hwnd);
    return w && w->cls ? w->cls->name : "";
}
void *rc_wnd_data(uint32_t hwnd)
{
    window *w = get(hwnd);
    return w ? w->data : NULL;
}
void rc_wnd_set_data(uint32_t hwnd, void *data)
{
    window *w = get(hwnd);
    if (w) {
        w->data = data;
    }
}
rc_scroll *rc_wnd_scroll(uint32_t hwnd, int bar)
{
    window *w = get(hwnd);
    return w && bar >= 0 && bar < 2 ? &w->sb[bar] : NULL;
}

/* The window's top-left on the screen. */
static void screen_origin(const window *w, int32_t *x, int32_t *y)
{
    int32_t ox = 0, oy = 0;
    if ((w->style & WS_CHILD) && get(w->parent)) {
        rc_wnd_client_origin(w->parent, &ox, &oy);
    }
    *x = ox + w->rect.l;
    *y = oy + w->rect.t;
}

void rc_wnd_client_origin(uint32_t hwnd, int32_t *x, int32_t *y)
{
    window *w = get(hwnd);
    *x = *y = 0;
    if (w) {
        screen_origin(w, x, y);
        *x += w->crect.l;
        *y += w->crect.t;
    }
}

rc_rect rc_wnd_client(uint32_t hwnd)
{
    window *w = get(hwnd);
    if (!w) {
        return (rc_rect){0, 0, 0, 0};
    }
    int32_t cw = w->crect.r - w->crect.l, ch = w->crect.b - w->crect.t;
    return (rc_rect){0, 0, cw > 0 ? cw : 0, ch > 0 ? ch : 0};
}

rc_rect rc_wnd_screen_rect(uint32_t hwnd)
{
    window *w = get(hwnd);
    if (!w) {
        return (rc_rect){0, 0, 0, 0};
    }
    int32_t x, y;
    screen_origin(w, &x, &y);
    return (rc_rect){x, y, x + w->rect.r - w->rect.l, y + w->rect.b - w->rect.t};
}

bool rc_wnd_visible(uint32_t hwnd)
{
    for (window *w = get(hwnd); w; w = (w->style & WS_CHILD) ? get(w->parent) : NULL) {
        if (!(w->style & WS_VISIBLE)) {
            return false;
        }
    }
    return get(hwnd) != NULL;
}

bool rc_wnd_enabled(uint32_t hwnd)
{
    window *w = get(hwnd);
    return w && !(w->style & WS_DISABLED);
}

uint32_t rc_wnd_top_level(uint32_t hwnd)
{
    window *w = get(hwnd);
    while (w && (w->style & WS_CHILD) && get(w->parent)) {
        w = get(w->parent);
    }
    return w ? handle_of(w) : 0;
}

/* Children of `parent` (0: the top-level windows) in z-order from the top; how many. */
static int siblings(uint32_t parent, uint32_t *out, int cap)
{
    int n = 0;
    for (int i = 0; i < WINDOWS && n < cap; i++) {
        window *w = &windows[i];
        if (!w->used) {
            continue;
        }
        bool child = (w->style & WS_CHILD) != 0;
        if ((parent && child && w->parent == parent) || (!parent && !child)) {
            out[n++] = handle_of(w);
        }
    }
    for (int a = 1; a < n; a++) {
        for (int b = a; b > 0 && get(out[b - 1])->z < get(out[b])->z; b--) {
            uint32_t t = out[b];
            out[b] = out[b - 1];
            out[b - 1] = t;
        }
    }
    return n;
}

uint32_t rc_wnd_first_child(uint32_t hwnd)
{
    uint32_t list[512];
    int n = siblings(hwnd, list, 512);
    return n ? list[0] : 0;
}

uint32_t rc_wnd_next_sibling(uint32_t hwnd)
{
    window *w = get(hwnd);
    if (!w) {
        return 0;
    }
    uint32_t list[512];
    int n = siblings((w->style & WS_CHILD) ? w->parent : 0, list, 512);
    for (int i = 0; i + 1 < n; i++) {
        if (list[i] == hwnd) {
            return list[i + 1];
        }
    }
    return 0;
}

/* ---- the desktop ---- */

static uint32_t fb; /* guest pixels, 32 bits, desktop_w x desktop_h */
static int32_t desktop_w, desktop_h;
static bool desktop_on, desktop_dirty, desktop_bg;
static uint64_t last_present;
static unsigned desktop_frames;

bool rc_desktop_active(void) { return desktop_on && !rc_ddraw_has_primary(); }

/* Whether the program imports DirectDraw (a game, shown through it), from the executable's
 * import table (mapped at 0x400000, as every executable here is). */
static bool uses_ddraw(void)
{
    static int known = -1;
    if (known >= 0) {
        return known;
    }
    known = 1;
    uint32_t base = 0x00400000u;
    if (!rc_vm_committed(base, 0x40) || rc_ld16(base) != 0x5a4d) {
        return true;
    }
    uint32_t pe = base + rc_ld32(base + 0x3c), dir = rc_ld32(pe + 24 + 96 + 8);
    if (!dir) {
        return (known = 0) != 0;
    }
    known = 0;
    for (uint32_t d = base + dir; rc_ld32(d + 12); d += 20) {
        const char *name = rc_str(base + rc_ld32(d + 12));
        char low[16] = {0};
        for (int i = 0; i < 15 && name[i]; i++) {
            low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
        }
        if (!strcmp(low, "ddraw.dll")) {
            known = 1;
        }
    }
    return known != 0;
}

void rc_desktop_size(int32_t *w, int32_t *h)
{
    if (!desktop_w) {
        desktop_w = 1024;
        desktop_h = 768;
        const char *e = getenv("RC_DESKTOP");
        int a, b;
        if (e && sscanf(e, "%dx%d", &a, &b) == 2 && a >= 640 && b >= 480 && a <= 4096 && b <= 4096) {
            desktop_w = a;
            desktop_h = b;
        }
    }
    *w = desktop_w;
    *h = desktop_h;
}

/* A program whose windows are not shown through DirectDraw: start the desktop. */
static void desktop_start(void)
{
    if (desktop_on || rc_ddraw_has_primary()) {
        return;
    }
    int32_t w, h;
    rc_desktop_size(&w, &h);
    fb = rc_heap_alloc((uint32_t)(w * h * 4), true);
    desktop_on = true;
    desktop_bg = true;
    rc_plat_window(w, h);
    rc_plat_show(true);
}

void rc_desktop_present(bool force)
{
    if (!rc_desktop_active()) {
        return;
    }
    uint64_t now = rc_ticks_peek_ms();
    if (!force && now - last_present < 16) {
        return;
    }
    last_present = now;
    desktop_dirty = false;
    rc_plat_present((const uint32_t *)rc_ptr(fb), desktop_w, desktop_h, desktop_w); /* pitch in pixels */
    rc_present_hook(++desktop_frames);
}

/* ---- device contexts ---- */

/* A window's area on the screen: its client area or the whole window. */
static rc_rect screen_area(const window *w, bool whole)
{
    int32_t x, y;
    screen_origin(w, &x, &y);
    if (whole) {
        return (rc_rect){x, y, x + w->rect.r - w->rect.l, y + w->rect.b - w->rect.t};
    }
    return (rc_rect){x + w->crect.l, y + w->crect.t, x + w->crect.r, y + w->crect.b};
}

/* Where a window may draw on the screen: its area within every parent's client area, without
 * the top-level windows above its own, its children (WS_CLIPCHILDREN) and the siblings above it
 * (WS_CLIPSIBLINGS). */
static void visible_region(window *w, bool whole, rc_rgn *g)
{
    rc_rect r = screen_area(w, whole);
    rc_rgn_set(g, r.l, r.t, r.r, r.b);
    uint32_t list[512];
    if (!whole && (w->style & WS_CLIPCHILDREN)) {
        int n = siblings(handle_of(w), list, 512);
        for (int i = 0; i < n; i++) {
            window *k = get(list[i]);
            if (k->style & WS_VISIBLE) {
                rc_rgn_cut_rect(g, screen_area(k, true));
            }
        }
    }
    for (window *k = w; k;) {
        bool child = (k->style & WS_CHILD) != 0;
        window *p = child ? get(k->parent) : NULL;
        if (!child || (k->style & WS_CLIPSIBLINGS)) {
            int n = siblings(child ? k->parent : 0, list, 512);
            for (int i = 0; i < n && list[i] != handle_of(k); i++) {
                window *s = get(list[i]);
                if (s->style & WS_VISIBLE) {
                    rc_rgn_cut_rect(g, screen_area(s, true));
                }
            }
        }
        if (p) {
            rc_rgn_and_rect(g, screen_area(p, false));
        }
        k = p;
    }
    rc_rgn_and_rect(g, (rc_rect){0, 0, desktop_w, desktop_h});
}

/* Window DCs of a game are stand-ins (its drawing is DirectDraw's). */
#define DC_BASE 0x01000000u

uint32_t rc_window_dc(uint32_t hwnd) { return DC_BASE + (hwnd & 0xffffu); }

/* A window DC's origin and visible region, as the windows are now. */
static void refresh_dc(rc_dc *d)
{
    window *w = get(d->hwnd);
    d->vis_gen = vis_gen;
    if (!w) {
        rc_rgn_empty(&d->vis);
        return;
    }
    bool whole = d->vis_kind == 2;
    rc_rect a = screen_area(w, whole);
    d->org_x = a.l;
    d->org_y = a.t;
    if (rc_wnd_visible(d->hwnd)) {
        visible_region(w, whole, &d->vis);
    } else {
        rc_rgn_empty(&d->vis);
    }
}

static void refresh_if_stale(rc_dc *d)
{
    if (d->vis_gen != vis_gen) {
        refresh_dc(d);
    }
}

uint32_t rc_wnd_dc(uint32_t hwnd, bool whole)
{
    window *w = get(hwnd);
    if (!rc_desktop_active()) {
        return rc_window_dc(hwnd);
    }
    uint32_t hdc = rc_dc_new();
    rc_dc *d = rc_dc_get(hdc);
    d->target = (rc_pixels){fb, (uint32_t)desktop_w, (uint32_t)desktop_h, (uint32_t)desktop_w * 4, 32, 0xff0000, 0xff00, 0xff};
    d->hwnd = hwnd ? hwnd : 0xffffffffu;
    if (w) {
        d->vis_kind = whole ? 2 : 1;
        rc_dc_refresh = refresh_if_stale;
        refresh_dc(d);
        d->font = w->font;
    } else {
        rc_rgn_set(&d->vis, 0, 0, desktop_w, desktop_h);
    }
    desktop_dirty = true;
    return hdc;
}

void rc_wnd_release_dc(uint32_t hdc)
{
    rc_dc *d = rc_dc_get(hdc);
    if (d && d->hwnd) {
        rc_dc_free(hdc);
    }
}

/* ---- messages ----
 * A message sent to a window of another thread is handled by that thread, as Windows does (MFC
 * keeps its window objects per thread): it waits here until that thread reads its messages. */

typedef struct sent {
    uint32_t hwnd, msg, wp, lp, result, thread;
    bool done, taken;
} sent;
#define SENT 64
static sent *sents[SENT];

uint32_t rc_wnd_thread(uint32_t hwnd)
{
    window *w = get(hwnd);
    return w ? w->thread : 0;
}

static uint32_t call_proc(rc_cpu *c, window *w, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

void rc_wnd_process_sent(rc_cpu *c)
{
    uint32_t me = rc_thread_id();
    for (int i = 0; i < SENT; i++) {
        sent *s = sents[i];
        if (s && !s->taken && s->thread == me) {
            s->taken = true;
            window *w = get(s->hwnd);
            s->result = w && w->wndproc ? call_proc(c, w, s->hwnd, s->msg, s->wp, s->lp) : 0;
            s->done = true;
        }
    }
}

uint32_t rc_wnd_send(rc_cpu *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    window *w = get(hwnd);
    if (!w || !w->wndproc) {
        return 0;
    }
    uint32_t me = rc_thread_id();
    if (w->thread && w->thread != me) {
        sent s = {hwnd, msg, wp, lp, 0, w->thread, false, false};
        int slot = -1;
        for (int i = 0; i < SENT && slot < 0; i++) {
            if (!sents[i]) {
                sents[slot = i] = &s;
            }
        }
        if (slot >= 0) {
            for (uint32_t spins = 0; !s.done && get(hwnd) && spins < 20000; spins++) {
                rc_wnd_process_sent(c); /* sends to this thread meanwhile */
                rc_thread_sleep(1);     /* the other thread may be waiting for time to pass */
            }
            sents[slot] = NULL;
            if (s.done) {
                return s.result;
            }
            if (s.taken) {
                return 0;
            }
            rc_log("user32: thread %u did not take message %#x for %#x; handled here", s.thread, msg, hwnd);
        }
        w = get(hwnd);
        if (!w) {
            return 0;
        }
    }
    return call_proc(c, w, hwnd, msg, wp, lp);
}

static uint32_t call_proc(rc_cpu *c, window *w, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    uint32_t args[4] = {hwnd, msg, wp, lp};
    uint32_t outer[4];
    const char *outer_what = rc_fault_note_get(outer);
    rc_fault_note("window message", hwnd, msg, wp, lp);
    uint32_t r = rc_call_guest(c, w->wndproc, 4, args);
    rc_fault_note_set(outer_what, outer);
    return r;
}

static uint32_t send(rc_cpu *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    return rc_wnd_send(c, hwnd, msg, wp, lp);
}

static uint32_t mkpoint(int32_t x, int32_t y) { return ((uint32_t)y << 16) | ((uint32_t)x & 0xffff); }

/* ---- painting ---- */

void rc_wnd_invalidate(uint32_t hwnd, const rc_rect *r, bool erase)
{
    window *w = get(hwnd);
    if (!w) {
        return;
    }
    rc_rect c = rc_wnd_client(hwnd), add = r ? rc_rect_and(*r, c) : c;
    if (!rc_rect_empty(add)) {
        if (rc_rect_empty(w->upd)) {
            w->upd = add;
        } else {
            w->upd.l = add.l < w->upd.l ? add.l : w->upd.l;
            w->upd.t = add.t < w->upd.t ? add.t : w->upd.t;
            w->upd.r = add.r > w->upd.r ? add.r : w->upd.r;
            w->upd.b = add.b > w->upd.b ? add.b : w->upd.b;
        }
        w->erase |= erase;
    }
    /* Children over the area paint again after the parent. */
    uint32_t list[512];
    int n = siblings(hwnd, list, 512);
    int32_t ox, oy;
    rc_wnd_client_origin(hwnd, &ox, &oy);
    rc_rect area = {add.l + ox, add.t + oy, add.r + ox, add.b + oy};
    for (int i = 0; i < n; i++) {
        window *k = get(list[i]);
        if (!(k->style & WS_VISIBLE)) {
            continue;
        }
        rc_rect x = rc_rect_and(screen_area(k, true), area);
        if (!rc_rect_empty(x)) {
            k->nc_dirty = true;
            int32_t cx, cy;
            rc_wnd_client_origin(list[i], &cx, &cy);
            rc_rect cr = {x.l - cx, x.t - cy, x.r - cx, x.b - cy};
            rc_wnd_invalidate(list[i], &cr, true);
        }
    }
}

static void invalidate_frame(window *w)
{
    w->nc_dirty = true;
    rc_wnd_invalidate(handle_of(w), NULL, true);
}

static void invalidate_all(void)
{
    desktop_bg = true;
    for (int i = 0; i < WINDOWS; i++) {
        if (windows[i].used && !(windows[i].style & WS_CHILD)) {
            invalidate_frame(&windows[i]);
        }
    }
}

/* The first window (parents first, lower z first) that needs painting. */
static uint32_t due_in(uint32_t parent)
{
    uint32_t list[512];
    int n = siblings(parent, list, 512);
    uint32_t me = rc_thread_id();
    for (int i = n - 1; i >= 0; i--) {
        window *w = get(list[i]);
        if (!(w->style & WS_VISIBLE) || w->redraw_off) {
            continue;
        }
        if ((w->nc_dirty || !rc_rect_empty(w->upd)) && w->thread == me) {
            return list[i];
        }
        uint32_t k = due_in(list[i]);
        if (k) {
            return k;
        }
    }
    return 0;
}

uint32_t rc_wnd_paint_due(uint32_t hwnd)
{
    if (hwnd) {
        window *w = get(hwnd);
        if (!w || !rc_wnd_visible(hwnd)) {
            return 0; /* nothing is painted in a window that is not shown (with all its parents) */
        }
        if ((w->nc_dirty || !rc_rect_empty(w->upd)) && w->thread == rc_thread_id()) {
            return hwnd;
        }
        return due_in(hwnd);
    }
    return due_in(0);
}

static void paint_desktop_bg(void)
{
    if (!desktop_bg || !rc_desktop_active()) {
        return;
    }
    desktop_bg = false;
    for (int32_t i = 0; i < desktop_w * desktop_h; i++) {
        rc_st32(fb + 4 * (uint32_t)i, 0x3a6ea5u); /* the classic desktop's blue */
    }
    desktop_dirty = true;
}

/* The frame of a window that needs it: sent now, before its WM_PAINT. */
static void paint_frame(rc_cpu *c, window *w)
{
    paint_desktop_bg();
    if (w->nc_dirty) {
        w->nc_dirty = false;
        send(c, handle_of(w), WM_NCPAINT, 1, 0);
    }
}

/* Paint what is due within `hwnd` (0: everything): frames now; client areas with WM_PAINT sent
 * now (`send_paint`) or handed back for the message queue to post (returns the window). */
static uint32_t paint_some(rc_cpu *c, uint32_t hwnd, bool send_paint)
{
    for (int guard = 0; guard < 4096; guard++) {
        uint32_t h = rc_wnd_paint_due(hwnd);
        window *w = get(h);
        if (!w) {
            return 0;
        }
        paint_frame(c, w);
        if (get(h) != w || rc_rect_empty(w->upd)) {
            continue;
        }
        if (!send_paint) {
            return h;
        }
        send(c, h, WM_PAINT, 0, 0);
        if (get(h) == w && !rc_rect_empty(w->upd)) {
            w->upd = (rc_rect){0, 0, 0, 0}; /* not validated: as DefWindowProc would */
        }
    }
    return 0;
}

void rc_wnd_paint_all(rc_cpu *c) { paint_some(c, 0, true); }
void rc_wnd_validate(uint32_t hwnd)
{
    window *w = get(hwnd);
    if (w) {
        w->upd = (rc_rect){0, 0, 0, 0};
        w->erase = false;
    }
}
void rc_wnd_update(rc_cpu *c, uint32_t hwnd) { paint_some(c, hwnd, true); }
uint32_t rc_wnd_paint_next(rc_cpu *c, uint32_t hwnd) { return paint_some(c, hwnd, false); }

/* ---- frames ---- */

static int32_t border_of(const window *w)
{
    if (w->game) {
        return 0;
    }
    if (w->style & WS_THICKFRAME) {
        return 4;
    }
    if ((w->style & WS_DLGFRAME) || (w->exstyle & WS_EX_DLGMODALFRAME)) {
        return 3;
    }
    return (w->style & WS_BORDER) ? 1 : 0;
}

static int32_t caption_of(const window *w)
{
    if (w->game || (w->style & WS_CAPTION) != WS_CAPTION) {
        return 0;
    }
    return (w->exstyle & WS_EX_TOOLWINDOW) ? 16 : 19;
}

/* The client area within the window rectangle `r`, as DefWindowProc computes it. */
static rc_rect default_client(const window *w, rc_rect r)
{
    int32_t b = border_of(w);
    r.l += b;
    r.t += b + caption_of(w);
    r.r -= b;
    r.b -= b;
    if (w->game && !(w->style & WS_CHILD) && w->menu) {
        /* A game window with a menu bar (Heroes IV) is placed by AdjustWindowRectEx with room for
         * the bar above the screen; the bar is not drawn, but its room counts, so the client
         * area is the screen where the program put it. */
        r.t += 20;
    }
    if (!w->game) {
        if (!(w->style & WS_CHILD) && w->menu) {
            r.t += rc_menu_bar_height(handle_of(w));
        }
        int32_t e = (w->exstyle & WS_EX_CLIENTEDGE) ? 2 : (w->exstyle & WS_EX_STATICEDGE) ? 1 : 0;
        r.l += e;
        r.t += e;
        r.r -= e;
        r.b -= e;
        if (w->style & WS_VSCROLL) {
            r.r -= 16;
        }
        if (w->style & WS_HSCROLL) {
            r.b -= 16;
        }
    }
    if (r.r < r.l) {
        r.r = r.l;
    }
    if (r.b < r.t) {
        r.b = r.t;
    }
    return r;
}

static void put_rect(uint32_t p, int32_t l, int32_t t, int32_t r, int32_t b)
{
    rc_st32(p, (uint32_t)l);
    rc_st32(p + 4, (uint32_t)t);
    rc_st32(p + 8, (uint32_t)r);
    rc_st32(p + 12, (uint32_t)b);
}

static void put_rect_r(uint32_t p, rc_rect r) { put_rect(p, r.l, r.t, r.r, r.b); }

static rc_rect get_rect(uint32_t p)
{
    return (rc_rect){(int32_t)rc_ld32(p), (int32_t)rc_ld32(p + 4), (int32_t)rc_ld32(p + 8), (int32_t)rc_ld32(p + 12)};
}

/* The client area after a change of the window rectangle: WM_NCCALCSIZE. */
static void calc_client(rc_cpu *c, window *w)
{
    uint32_t p = rc_heap_alloc(16, false);
    put_rect_r(p, w->rect);
    if (w->wndproc) {
        send(c, handle_of(w), WM_NCCALCSIZE, 0, p);
    } else {
        put_rect_r(p, default_client(w, w->rect));
    }
    rc_rect r = get_rect(p);
    rc_heap_free(p);
    w->crect = (rc_rect){r.l - w->rect.l, r.t - w->rect.t, r.r - w->rect.l, r.b - w->rect.t};
}

/* ---- creating and destroying ---- */

static uint32_t create(rc_cpu *c, uint32_t exstyle, uint32_t cls_arg, const char *cls_name, uint32_t title_arg,
                       const char *title, uint32_t style, int32_t x, int32_t y, int32_t wd, int32_t ht, uint32_t parent,
                       uint32_t menu, uint32_t inst, uint32_t param)
{
    rc_wclass *k = rc_class_find(cls_name);
    if (!k) {
        rc_log("CreateWindowExA: no window class \"%s\"", cls_name);
        rc_last_error = 1407; /* ERROR_CANNOT_FIND_WND_CLASS */
        return 0;
    }
    uint32_t i = 0;
    while (i < WINDOWS && windows[i].used) {
        i++;
    }
    if (i == WINDOWS) {
        rc_log("CreateWindowExA: out of windows");
        return 0;
    }
    window *w = &windows[i];
    memset(w, 0, sizeof *w);
    w->used = true;
    vis_gen++;
    w->cls = k;
    w->wndproc = k->wndproc;
    w->style = style & ~WS_VISIBLE;
    w->exstyle = exstyle;
    w->instance = inst;
    w->thread = rc_thread_id();
    bool child = (style & WS_CHILD) != 0;
    if (child) {
        w->parent = parent;
        w->id = menu;
        w->z = --z_bottom; /* below its siblings: tab order is creation order */
    } else {
        /* a top-level window's owner is the top-level window of what was passed */
        w->owner = parent ? rc_wnd_top_level(parent) : 0;
        w->parent = w->owner;
        /* No menu given: the class's menu, as Windows loads it (Heroes IV's game window). */
        w->menu = menu ? menu : k->menu_name ? rc_menu_load(k->instance ? k->instance : inst, k->menu_name) : 0;
        w->z = ++z_top;
        vis_gen++;
    }
    int32_t dw, dh;
    rc_desktop_size(&dw, &dh);
    if (x == (int32_t)CW_USEDEFAULT) {
        x = child ? 0 : 24 * (int32_t)(i % 8);
        y = child ? 0 : 24 * (int32_t)(i % 8);
    }
    if (wd == (int32_t)CW_USEDEFAULT) {
        wd = child ? 0 : dw * 3 / 4;
        ht = child ? 0 : dh * 3 / 4;
    }
    w->rect = (rc_rect){x, y, x + wd, y + ht};
    vis_gen++;
    w->text = strdup(title ? title : "");
    uint32_t hwnd = HWND_BASE + 4 * i;
    if (!child && !main_hwnd) {
        main_hwnd = hwnd;
        rc_plat_title(title ? title : "");
    }
    if (!uses_ddraw()) {
        desktop_start(); /* a program that draws its windows with GDI */
    }

    /* CREATESTRUCTA, as the CBT hook, WM_NCCREATE and WM_CREATE see it. */
    uint32_t cs = rc_heap_alloc(48, true), cbt = rc_heap_alloc(8, true);
    rc_st32(cs, param);
    rc_st32(cs + 4, inst);
    rc_st32(cs + 8, menu);
    rc_st32(cs + 12, parent);
    rc_st32(cs + 16, (uint32_t)ht);
    rc_st32(cs + 20, (uint32_t)wd);
    rc_st32(cs + 24, (uint32_t)y);
    rc_st32(cs + 28, (uint32_t)x);
    rc_st32(cs + 32, style);
    rc_st32(cs + 36, title_arg);
    rc_st32(cs + 40, cls_arg);
    rc_st32(cs + 44, exstyle);
    rc_st32(cbt, cs);
    bool called = false;
    uint32_t veto = rc_call_hooks(c, 5 /* WH_CBT */, 3 /* HCBT_CREATEWND */, hwnd, cbt, &called);
    uint32_t ok = (called && veto) ? 0 : send(c, hwnd, WM_NCCREATE, 0, cs);
    int32_t created = -1;
    if (ok && get(hwnd) == w) {
        /* the size the window was given, which the CBT hook or WM_NCCREATE may have changed */
        int32_t nx = (int32_t)rc_ld32(cs + 28), ny = (int32_t)rc_ld32(cs + 24);
        w->rect = (rc_rect){nx, ny, nx + (int32_t)rc_ld32(cs + 20), ny + (int32_t)rc_ld32(cs + 16)};
        vis_gen++;
        calc_client(c, w);
        created = (int32_t)send(c, hwnd, WM_CREATE, 0, cs);
    }
    rc_heap_free(cs);
    rc_heap_free(cbt);
    if (created == -1 || get(hwnd) != w) {
        if (get(hwnd) == w) {
            send(c, hwnd, WM_NCDESTROY, 0, 0);
            free(w->text);
            w->used = false;
            vis_gen++;
        }
        if (main_hwnd == hwnd) {
            main_hwnd = 0;
        }
        return 0;
    }
    send(c, hwnd, WM_SIZE, 0, mkpoint(w->crect.r - w->crect.l, w->crect.b - w->crect.t));
    send(c, hwnd, WM_MOVE, 0, mkpoint(w->rect.l + w->crect.l, w->rect.t + w->crect.t));
    if (child && !(exstyle & 0x4u /* WS_EX_NOPARENTNOTIFY */) && get(parent)) {
        send(c, parent, WM_PARENTNOTIFY, WM_CREATE | (w->id << 16), hwnd);
    }
    if ((style & WS_VISIBLE) && get(hwnd) == w) {
        rc_wnd_show(c, hwnd, (style & WS_MAXIMIZE) ? 3 : 5);
    }
    return get(hwnd) == w ? hwnd : 0;
}

uint32_t rc_wnd_create(rc_cpu *c, uint32_t exstyle, const char *cls, const char *text, uint32_t style, int32_t x,
                       int32_t y, int32_t w, int32_t h, uint32_t parent, uint32_t menu, uint32_t instance,
                       uint32_t param)
{
    uint32_t gcls = rc_heap_alloc((uint32_t)strlen(cls) + 1, false);
    uint32_t gtext = rc_heap_alloc((uint32_t)strlen(text ? text : "") + 1, false);
    memcpy(rc_ptr(gcls), cls, strlen(cls) + 1);
    memcpy(rc_ptr(gtext), text ? text : "", strlen(text ? text : "") + 1);
    uint32_t r = create(c, exstyle, gcls, cls, gtext, text, style, x, y, w, h, parent, menu, instance, param);
    rc_heap_free(gcls);
    rc_heap_free(gtext);
    return r;
}

static void k_CreateWindowExA(rc_cpu *c)
{
    uint32_t exstyle = RC_ARG(c, 0), cls = RC_ARG(c, 1), title = RC_ARG(c, 2), style = RC_ARG(c, 3);
    char name[64];
    if (cls < 0x10000) {
        snprintf(name, sizeof name, "#%u", cls);
    } else {
        snprintf(name, sizeof name, "%s", rc_str(cls));
    }
    uint32_t hwnd = create(c, exstyle, cls, name, title, title ? rc_str(title) : "", style, (int32_t)RC_ARG(c, 4),
                           (int32_t)RC_ARG(c, 5), (int32_t)RC_ARG(c, 6), (int32_t)RC_ARG(c, 7), RC_ARG(c, 8),
                           RC_ARG(c, 9), RC_ARG(c, 10), RC_ARG(c, 11));
    RC_RETURN(c, hwnd, 12);
}

static void destroy_tree(rc_cpu *c, uint32_t hwnd, bool send_destroy)
{
    window *w = get(hwnd);
    if (!w) {
        return;
    }
    if (send_destroy) {
        send(c, hwnd, WM_DESTROY, 0, 0);
    }
    uint32_t list[512];
    int n = siblings(hwnd, list, 512);
    for (int i = 0; i < n; i++) {
        destroy_tree(c, list[i], true);
    }
    if (get(hwnd) != w) {
        return;
    }
    send(c, hwnd, WM_NCDESTROY, 0, 0);
    if (get(hwnd) != w) {
        return;
    }
    if (focus_hwnd == hwnd) {
        focus_hwnd = 0;
    }
    if (capture_hwnd == hwnd) {
        capture_hwnd = 0;
    }
    if (active_hwnd == hwnd) {
        active_hwnd = 0;
    }
    rc_input_forget(hwnd);
    free(w->text);
    free(w->data);
    w->used = false;
    vis_gen++;
    if (hwnd == main_hwnd) {
        main_hwnd = 0;
        if (!desktop_on) {
            rc_plat_show(false);
        }
    }
}

void rc_wnd_destroy(rc_cpu *c, uint32_t hwnd)
{
    window *w = get(hwnd);
    if (!w) {
        return;
    }
    bool called = false;
    if (rc_call_hooks(c, 5, 4 /* HCBT_DESTROYWND */, hwnd, 0, &called) && called) {
        return;
    }
    bool top = !(w->style & WS_CHILD);
    if (top) {
        for (int i = 0; i < WINDOWS; i++) { /* owned windows go first */
            if (windows[i].used && !(windows[i].style & WS_CHILD) && windows[i].owner == hwnd) {
                rc_wnd_destroy(c, handle_of(&windows[i]));
            }
        }
    }
    if (get(hwnd) != w) {
        return;
    }
    bool was_visible = (w->style & WS_VISIBLE) != 0;
    uint32_t parent = (w->style & WS_CHILD) ? w->parent : 0, id = w->id, owner = w->owner;
    if (parent && !(w->exstyle & 0x4u) && get(parent)) {
        send(c, parent, WM_PARENTNOTIFY, WM_DESTROY | (id << 16), hwnd);
    }
    if (get(hwnd) != w) {
        return;
    }
    w->style &= ~WS_VISIBLE;
    vis_gen++;
    destroy_tree(c, hwnd, true);
    if (was_visible) {
        if (parent && get(parent)) {
            rc_wnd_invalidate(parent, NULL, true);
        } else {
            invalidate_all();
        }
    }
    if (top && !active_hwnd) {
        /* the owner, or the topmost window left, becomes active */
        uint32_t next = owner && get(owner) && (get(owner)->style & WS_VISIBLE) ? owner : 0;
        uint32_t list[512];
        int n = siblings(0, list, 512);
        for (int i = 0; i < n && !next; i++) {
            if ((get(list[i])->style & WS_VISIBLE) && !(get(list[i])->style & WS_DISABLED)) {
                next = list[i];
            }
        }
        if (next) {
            rc_wnd_activate(c, next);
        }
    }
}

static void k_DestroyWindow(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0);
    if (!get(hwnd)) {
        RC_RETURN(c, 0, 1);
    }
    rc_wnd_destroy(c, hwnd);
    RC_RETURN(c, 1, 1);
}

/* ---- showing, activation, focus ---- */

uint32_t rc_wnd_active(void) { return active_hwnd; }
uint32_t rc_wnd_focus(void) { return focus_hwnd; }
uint32_t rc_wnd_capture(void) { return capture_hwnd; }
void rc_wnd_set_capture(uint32_t hwnd) { capture_hwnd = hwnd; }

uint32_t rc_wnd_set_focus(rc_cpu *c, uint32_t hwnd)
{
    uint32_t old = focus_hwnd;
    if (hwnd == old || (hwnd && !get(hwnd))) {
        return old;
    }
    bool called = false;
    if (rc_call_hooks(c, 5, 9 /* HCBT_SETFOCUS */, hwnd, old, &called) && called) {
        return 0;
    }
    if (hwnd) {
        uint32_t top = rc_wnd_top_level(hwnd);
        if (top != active_hwnd) {
            rc_wnd_activate(c, top);
            if (focus_hwnd == hwnd) {
                return old;
            }
        }
    }
    old = focus_hwnd;
    focus_hwnd = hwnd;
    if (old && get(old)) {
        send(c, old, WM_KILLFOCUS, hwnd, 0);
    }
    if (hwnd && focus_hwnd == hwnd && get(hwnd)) {
        send(c, hwnd, WM_SETFOCUS, old, 0);
    }
    return old;
}

void rc_wnd_activate(rc_cpu *c, uint32_t hwnd)
{
    hwnd = rc_wnd_top_level(hwnd);
    window *w = get(hwnd);
    if (!w) {
        return;
    }
    if (hwnd == active_hwnd) {
        if (!w->game && w->z != z_top) {
            w->z = ++z_top;
            vis_gen++;
            invalidate_frame(w);
        }
        return;
    }
    bool called = false;
    if (rc_call_hooks(c, 5, 5 /* HCBT_ACTIVATE */, hwnd, 0, &called) && called) {
        return;
    }
    uint32_t old = active_hwnd;
    if (old && get(old)) {
        send(c, old, WM_NCACTIVATE, 0, 0);
        send(c, old, WM_ACTIVATE, 0, hwnd);
        if (get(old)) {
            get(old)->nc_dirty = true;
        }
    } else {
        send(c, hwnd, WM_ACTIVATEAPP, 1, 0);
    }
    if (get(hwnd) != w) {
        return;
    }
    active_hwnd = hwnd;
    w->z = ++z_top;
    vis_gen++;
    send(c, hwnd, WM_NCACTIVATE, 1, 0);
    send(c, hwnd, WM_ACTIVATE, 1, old);
    if (get(hwnd) == w) {
        invalidate_frame(w);
    }
}

void rc_wnd_show(rc_cpu *c, uint32_t hwnd, int cmd)
{
    window *w = get(hwnd);
    if (!w) {
        return;
    }
    bool was = (w->style & WS_VISIBLE) != 0, show = cmd != 0;
    bool top = !(w->style & WS_CHILD);
    if (show && top && !w->game && !rc_ddraw_has_primary() &&
        ((w->style & (WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME)) || w->owner || desktop_on)) {
        desktop_start();
    }
    if (show && cmd == 3 /* SW_SHOWMAXIMIZED */ && top && !w->game) {
        int32_t dw, dh;
        rc_desktop_size(&dw, &dh);
        w->style |= WS_MAXIMIZE;
        rc_wnd_move(c, hwnd, 0, 0, dw, dh, false);
    }
    if (show && !was) {
        send(c, hwnd, WM_SHOWWINDOW, 1, 0);
        if (get(hwnd) != w) {
            return;
        }
        w->style |= WS_VISIBLE;
        vis_gen++;
        if (hwnd == main_hwnd && !desktop_on) {
            rc_plat_window(w->rect.r - w->rect.l, w->rect.b - w->rect.t);
            rc_plat_show(true);
        }
        invalidate_frame(w);
        if (top && cmd != 4 && cmd != 7 && cmd != 8) {
            rc_wnd_activate(c, hwnd);
            if (!focus_hwnd || rc_wnd_top_level(focus_hwnd) != hwnd) {
                rc_wnd_set_focus(c, hwnd);
            }
        }
        if (w->game || !desktop_on) {
            /* a game's window: as before the desktop existed */
            send(c, hwnd, WM_SIZE, 0, mkpoint(w->crect.r - w->crect.l, w->crect.b - w->crect.t));
            send(c, hwnd, WM_MOVE, 0, mkpoint(w->rect.l, w->rect.t));
        }
    } else if (!show && was) {
        send(c, hwnd, WM_SHOWWINDOW, 0, 0);
        if (get(hwnd) != w) {
            return;
        }
        w->style &= ~WS_VISIBLE;
        vis_gen++;
        if (top) {
            invalidate_all();
            if (hwnd == main_hwnd && !desktop_on) {
                rc_plat_show(false);
            }
            if (active_hwnd == hwnd) {
                active_hwnd = 0;
                if (w->owner && get(w->owner) && (get(w->owner)->style & WS_VISIBLE)) {
                    rc_wnd_activate(c, w->owner);
                }
            }
        } else if (get(w->parent)) {
            rc_wnd_invalidate(w->parent, NULL, true);
        }
        for (uint32_t f = focus_hwnd; f; f = rc_wnd_parent(f)) {
            if (f == hwnd) {
                focus_hwnd = rc_wnd_parent(hwnd);
                break;
            }
        }
    }
}

static void k_ShowWindow(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0);
    window *w = get(hwnd);
    if (!w) {
        RC_RETURN(c, 0, 2);
    }
    bool was = (w->style & WS_VISIBLE) != 0;
    int cmd = (int)RC_ARG(c, 1);
    rc_wnd_show(c, hwnd, cmd == 10 ? 5 : cmd);
    RC_RETURN(c, was, 2);
}

static void k_ShowOwnedPopups(rc_cpu *c)
{
    uint32_t owner = RC_ARG(c, 0), show = RC_ARG(c, 1);
    for (int i = 0; i < WINDOWS; i++) {
        if (windows[i].used && !(windows[i].style & WS_CHILD) && windows[i].owner == owner) {
            rc_wnd_show(c, handle_of(&windows[i]), show ? 8 : 0);
        }
    }
    RC_RETURN(c, 1, 2);
}

void rc_wnd_enable(rc_cpu *c, uint32_t hwnd, bool enable)
{
    window *w = get(hwnd);
    if (!w || !(w->style & WS_DISABLED) == enable) {
        return;
    }
    w->style = enable ? w->style & ~WS_DISABLED : w->style | WS_DISABLED;
    if (!enable && focus_hwnd == hwnd) {
        rc_wnd_set_focus(c, 0);
    }
    send(c, hwnd, WM_ENABLE, enable, 0);
    if (get(hwnd) == w) {
        invalidate_frame(w);
    }
}

static void k_EnableWindow(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    bool was_disabled = w && (w->style & WS_DISABLED);
    rc_wnd_enable(c, RC_ARG(c, 0), RC_ARG(c, 1) != 0);
    RC_RETURN(c, was_disabled, 2);
}

static void k_IsWindow(rc_cpu *c) { RC_RETURN(c, get(RC_ARG(c, 0)) != NULL, 1); }
static void k_IsWindowVisible(rc_cpu *c) { RC_RETURN(c, rc_wnd_visible(RC_ARG(c, 0)), 1); }
static void k_IsWindowEnabled(rc_cpu *c) { RC_RETURN(c, rc_wnd_enabled(RC_ARG(c, 0)), 1); }
static void k_IsIconic(rc_cpu *c) { RC_RETURN(c, 0, 1); }
static void k_IsZoomed(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    RC_RETURN(c, w && (w->style & WS_MAXIMIZE), 1);
}
static void k_IsWindowUnicode(rc_cpu *c) { RC_RETURN(c, 0, 1); }
static void k_SetFocus(rc_cpu *c) { RC_RETURN(c, rc_wnd_set_focus(c, RC_ARG(c, 0)), 1); }
static void k_GetFocus(rc_cpu *c) { RC_RETURN(c, focus_hwnd, 0); }
static void k_GetActiveWindow(rc_cpu *c) { RC_RETURN(c, active_hwnd, 0); }
static void k_SetActiveWindow(rc_cpu *c)
{
    uint32_t old = active_hwnd;
    rc_wnd_activate(c, RC_ARG(c, 0));
    RC_RETURN(c, old, 1);
}
static void k_GetForegroundWindow(rc_cpu *c) { RC_RETURN(c, active_hwnd ? active_hwnd : main_hwnd, 0); }
static void k_SetForegroundWindow(rc_cpu *c)
{
    rc_wnd_activate(c, RC_ARG(c, 0));
    RC_RETURN(c, 1, 1);
}
static void k_BringWindowToTop(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    if (w) {
        w->z = ++z_top;
        vis_gen++;
        invalidate_frame(w);
    }
    RC_RETURN(c, w != NULL, 1);
}
static void k_GetCapture(rc_cpu *c) { RC_RETURN(c, capture_hwnd, 0); }
static void k_SetCapture(rc_cpu *c)
{
    uint32_t old = capture_hwnd;
    capture_hwnd = RC_ARG(c, 0);
    RC_RETURN(c, old, 1);
}
static void k_ReleaseCapture(rc_cpu *c)
{
    capture_hwnd = 0;
    RC_RETURN(c, 1, 0);
}
static void k_GetLastActivePopup(rc_cpu *c)
{
    uint32_t owner = RC_ARG(c, 0);
    if (active_hwnd && get(active_hwnd) && get(active_hwnd)->owner == owner) {
        RC_RETURN(c, active_hwnd, 1);
    }
    RC_RETURN(c, owner, 1);
}

/* ---- the tree ---- */

#define DESKTOP_HWND 0x00010000u

static void k_GetParent(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    RC_RETURN(c, w ? ((w->style & WS_CHILD) ? w->parent : w->owner) : 0, 1);
}

static void k_SetParent(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    if (!w) {
        RC_RETURN(c, 0, 2);
    }
    uint32_t old = w->parent;
    w->parent = RC_ARG(c, 1) == DESKTOP_HWND ? 0 : RC_ARG(c, 1);
    w->z = (w->style & WS_CHILD) ? --z_bottom : ++z_top;
    vis_gen++;
    invalidate_all();
    RC_RETURN(c, old, 2);
}

/* GetWindow(hwnd, cmd): GW_HWNDFIRST 0, LAST 1, NEXT 2, PREV 3, OWNER 4, CHILD 5. */
static void k_GetWindow(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), cmd = RC_ARG(c, 1);
    window *w = get(hwnd);
    if (!w) {
        RC_RETURN(c, 0, 2);
    }
    if (cmd == 5) {
        RC_RETURN(c, rc_wnd_first_child(hwnd), 2);
    }
    if (cmd == 4) {
        RC_RETURN(c, (w->style & WS_CHILD) ? 0 : w->owner, 2);
    }
    uint32_t list[512];
    int n = siblings((w->style & WS_CHILD) ? w->parent : 0, list, 512), at = 0;
    while (at < n && list[at] != hwnd) {
        at++;
    }
    uint32_t r = 0;
    switch (cmd) {
    case 0: r = n ? list[0] : 0; break;
    case 1: r = n ? list[n - 1] : 0; break;
    case 2: r = at + 1 < n ? list[at + 1] : 0; break;
    case 3: r = at > 0 && at < n ? list[at - 1] : 0; break;
    default: break;
    }
    RC_RETURN(c, r, 2);
}

static void k_GetTopWindow(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0) == DESKTOP_HWND ? 0 : RC_ARG(c, 0);
    if (h) {
        RC_RETURN(c, rc_wnd_first_child(h), 1);
    }
    uint32_t list[512];
    int n = siblings(0, list, 512);
    RC_RETURN(c, n ? list[0] : 0, 1);
}

static void k_GetDesktopWindow(rc_cpu *c) { RC_RETURN(c, DESKTOP_HWND, 0); }

static void k_IsChild(rc_cpu *c)
{
    uint32_t parent = RC_ARG(c, 0);
    for (uint32_t h = rc_wnd_parent(RC_ARG(c, 1)); h; h = rc_wnd_parent(h)) {
        if (h == parent) {
            RC_RETURN(c, 1, 2);
        }
    }
    RC_RETURN(c, 0, 2);
}

static void k_GetDlgCtrlID(rc_cpu *c) { RC_RETURN(c, rc_wnd_id(RC_ARG(c, 0)), 1); }

static void k_GetClassNameA(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    if (!w) {
        RC_RETURN(c, 0, 3);
    }
    RC_RETURN(c, rc_put_str(RC_ARG(c, 1), RC_ARG(c, 2), w->cls->name), 3);
}

/* FindWindowA(class, title) */
static void k_FindWindowA(rc_cpu *c)
{
    const char *cls = RC_ARG(c, 0) >= 0x10000 ? rc_str(RC_ARG(c, 0)) : NULL;
    const char *title = RC_ARG(c, 1) ? rc_str(RC_ARG(c, 1)) : NULL;
    for (int i = 0; i < WINDOWS; i++) {
        window *w = &windows[i];
        if (!w->used || (w->style & WS_CHILD)) {
            continue;
        }
        if ((cls && strcmp(w->cls->name, cls)) || (title && strcmp(w->text ? w->text : "", title))) {
            continue;
        }
        RC_RETURN(c, handle_of(w), 2);
    }
    RC_RETURN(c, 0, 2);
}

static bool enum_children(rc_cpu *c, uint32_t parent, uint32_t proc, uint32_t lp)
{
    uint32_t list[512];
    int n = siblings(parent, list, 512);
    for (int i = 0; i < n; i++) {
        if (!get(list[i])) {
            continue;
        }
        uint32_t args[2] = {list[i], lp};
        if (!rc_call_guest(c, proc, 2, args) || !enum_children(c, list[i], proc, lp)) {
            return false;
        }
    }
    return true;
}

static void k_EnumChildWindows(rc_cpu *c)
{
    enum_children(c, RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2));
    RC_RETURN(c, 1, 3);
}

static void k_EnumWindows(rc_cpu *c)
{
    uint32_t list[512];
    int n = siblings(0, list, 512);
    for (int i = 0; i < n; i++) {
        uint32_t args[2] = {list[i], RC_ARG(c, 1)};
        if (get(list[i]) && !rc_call_guest(c, RC_ARG(c, 0), 2, args)) {
            break;
        }
    }
    RC_RETURN(c, 1, 2);
}

/* ---- text ---- */

static void k_SetWindowTextA(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0);
    if (!get(hwnd)) {
        RC_RETURN(c, 0, 2);
    }
    send(c, hwnd, WM_SETTEXT, 0, RC_ARG(c, 1));
    RC_RETURN(c, 1, 2);
}

static void k_GetWindowTextA(rc_cpu *c)
{
    if (!get(RC_ARG(c, 0)) || !RC_ARG(c, 2)) {
        RC_RETURN(c, 0, 3);
    }
    RC_RETURN(c, send(c, RC_ARG(c, 0), WM_GETTEXT, RC_ARG(c, 2), RC_ARG(c, 1)), 3);
}

static void k_GetWindowTextLengthA(rc_cpu *c)
{
    RC_RETURN(c, get(RC_ARG(c, 0)) ? send(c, RC_ARG(c, 0), WM_GETTEXTLENGTH, 0, 0) : 0, 1);
}

/* ---- positions ---- */

static void k_GetClientRect(rc_cpu *c)
{
    if (!get(RC_ARG(c, 0))) {
        RC_RETURN(c, 0, 2);
    }
    put_rect_r(RC_ARG(c, 1), rc_wnd_client(RC_ARG(c, 0)));
    RC_RETURN(c, 1, 2);
}

static void k_GetWindowRect(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0);
    if (hwnd == DESKTOP_HWND) {
        int32_t w, h;
        rc_desktop_size(&w, &h);
        put_rect(RC_ARG(c, 1), 0, 0, w, h);
        RC_RETURN(c, 1, 2);
    }
    if (!get(hwnd)) {
        RC_RETURN(c, 0, 2);
    }
    put_rect_r(RC_ARG(c, 1), rc_wnd_screen_rect(hwnd));
    RC_RETURN(c, 1, 2);
}

static void k_ClientToScreen(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 1);
    int32_t x, y;
    rc_wnd_client_origin(RC_ARG(c, 0), &x, &y);
    rc_st32(p, rc_ld32(p) + (uint32_t)x);
    rc_st32(p + 4, rc_ld32(p + 4) + (uint32_t)y);
    RC_RETURN(c, 1, 2);
}

static void k_ScreenToClient(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 1);
    int32_t x, y;
    rc_wnd_client_origin(RC_ARG(c, 0), &x, &y);
    rc_st32(p, rc_ld32(p) - (uint32_t)x);
    rc_st32(p + 4, rc_ld32(p + 4) - (uint32_t)y);
    RC_RETURN(c, 1, 2);
}

/* MapWindowPoints(from, to, points, n): the offset in the low and high word of the result. */
static void k_MapWindowPoints(rc_cpu *c)
{
    int32_t fx = 0, fy = 0, tx = 0, ty = 0;
    if (get(RC_ARG(c, 0))) {
        rc_wnd_client_origin(RC_ARG(c, 0), &fx, &fy);
    }
    if (get(RC_ARG(c, 1))) {
        rc_wnd_client_origin(RC_ARG(c, 1), &tx, &ty);
    }
    int32_t dx = fx - tx, dy = fy - ty;
    uint32_t p = RC_ARG(c, 2), n = RC_ARG(c, 3);
    for (uint32_t i = 0; i < n; i++) {
        rc_st32(p + 8 * i, rc_ld32(p + 8 * i) + (uint32_t)dx);
        rc_st32(p + 8 * i + 4, rc_ld32(p + 8 * i + 4) + (uint32_t)dy);
    }
    RC_RETURN(c, ((uint32_t)(dy & 0xffff) << 16) | (uint32_t)(dx & 0xffff), 4);
}

#define SWP_NOSIZE 0x1u
#define SWP_NOMOVE 0x2u
#define SWP_NOZORDER 0x4u
#define SWP_NOREDRAW 0x8u
#define SWP_NOACTIVATE 0x10u
#define SWP_FRAMECHANGED 0x20u
#define SWP_SHOWWINDOW 0x40u
#define SWP_HIDEWINDOW 0x80u

/* SetWindowPos's work: WM_WINDOWPOSCHANGING, the new rectangle, WM_NCCALCSIZE, WM_WINDOWPOSCHANGED. */
static void set_pos(rc_cpu *c, uint32_t hwnd, uint32_t after, int32_t x, int32_t y, int32_t cx, int32_t cy,
                    uint32_t flags)
{
    window *w = get(hwnd);
    if (!w) {
        return;
    }
    uint32_t wp = rc_heap_alloc(28, true);
    rc_st32(wp, hwnd);
    rc_st32(wp + 4, after);
    rc_st32(wp + 8, (uint32_t)((flags & SWP_NOMOVE) ? w->rect.l : x));
    rc_st32(wp + 12, (uint32_t)((flags & SWP_NOMOVE) ? w->rect.t : y));
    rc_st32(wp + 16, (uint32_t)((flags & SWP_NOSIZE) ? w->rect.r - w->rect.l : cx));
    rc_st32(wp + 20, (uint32_t)((flags & SWP_NOSIZE) ? w->rect.b - w->rect.t : cy));
    rc_st32(wp + 24, flags);
    bool change = !(flags & SWP_NOMOVE) || !(flags & SWP_NOSIZE) || (flags & SWP_FRAMECHANGED);
    if (change || !(flags & SWP_NOZORDER)) {
        send(c, hwnd, WM_WINDOWPOSCHANGING, 0, wp);
    }
    if (get(hwnd) != w) {
        rc_heap_free(wp);
        return;
    }
    flags = rc_ld32(wp + 24);
    rc_rect old = rc_wnd_screen_rect(hwnd), old_client = w->crect;
    if (!(flags & SWP_NOMOVE) || !(flags & SWP_NOSIZE)) {
        int32_t nx = (int32_t)rc_ld32(wp + 8), ny = (int32_t)rc_ld32(wp + 12);
        int32_t nw = (int32_t)rc_ld32(wp + 16), nh = (int32_t)rc_ld32(wp + 20);
        w->rect = (rc_rect){nx, ny, nx + (nw > 0 ? nw : 0), ny + (nh > 0 ? nh : 0)};
        vis_gen++;
    }
    if (change) {
        calc_client(c, w);
    }
    if (!(flags & SWP_NOZORDER)) {
        if (after == 0 || after == 0xffffffffu) { /* HWND_TOP, HWND_TOPMOST */
            w->z = ++z_top;
            vis_gen++;
        } else if (after == 1) { /* HWND_BOTTOM */
            w->z = --z_bottom;
            vis_gen++;
        } else if (get(after)) {
            w->z = get(after)->z - 1;
            vis_gen++;
        }
    }
    if (change || !(flags & SWP_NOZORDER)) {
        send(c, hwnd, WM_WINDOWPOSCHANGED, 0, wp);
    }
    rc_heap_free(wp);
    if (get(hwnd) != w) {
        return;
    }
    if (flags & SWP_SHOWWINDOW) {
        rc_wnd_show(c, hwnd, (flags & SWP_NOACTIVATE) ? 8 : 5);
    } else if (flags & SWP_HIDEWINDOW) {
        rc_wnd_show(c, hwnd, 0);
    }
    if (get(hwnd) == w && (change || !(flags & SWP_NOZORDER)) && (w->style & WS_VISIBLE)) {
        rc_rect now = rc_wnd_screen_rect(hwnd);
        bool moved = memcmp(&old, &now, sizeof old) != 0 || memcmp(&old_client, &w->crect, sizeof old_client) != 0;
        if (w->style & WS_CHILD) {
            if (moved && get(w->parent)) {
                int32_t px, py;
                rc_wnd_client_origin(w->parent, &px, &py);
                rc_rect a = {old.l - px, old.t - py, old.r - px, old.b - py};
                rc_wnd_invalidate(w->parent, &a, true);
            }
            if (moved || (flags & SWP_FRAMECHANGED) || !(flags & SWP_NOZORDER)) {
                invalidate_frame(w);
            }
        } else if (moved) {
            invalidate_all();
        } else {
            invalidate_frame(w);
        }
    }
}

void rc_wnd_frame_changed(rc_cpu *c, uint32_t hwnd)
{
    set_pos(c, hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void rc_wnd_redraw_frame(uint32_t hwnd)
{
    window *w = get(hwnd);
    if (w) {
        w->nc_dirty = true;
    }
}

void rc_wnd_move(rc_cpu *c, uint32_t hwnd, int32_t x, int32_t y, int32_t w, int32_t h, bool repaint)
{
    set_pos(c, hwnd, 0, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | (repaint ? 0 : SWP_NOREDRAW));
}

static void k_SetWindowPos(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0);
    if (!get(hwnd)) {
        RC_RETURN(c, 0, 7);
    }
    set_pos(c, hwnd, RC_ARG(c, 1), (int32_t)RC_ARG(c, 2), (int32_t)RC_ARG(c, 3), (int32_t)RC_ARG(c, 4),
            (int32_t)RC_ARG(c, 5), RC_ARG(c, 6));
    RC_RETURN(c, 1, 7);
}

static void k_MoveWindow(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0);
    if (!get(hwnd)) {
        RC_RETURN(c, 0, 6);
    }
    rc_wnd_move(c, hwnd, (int32_t)RC_ARG(c, 1), (int32_t)RC_ARG(c, 2), (int32_t)RC_ARG(c, 3), (int32_t)RC_ARG(c, 4),
                RC_ARG(c, 5) != 0);
    RC_RETURN(c, 1, 6);
}

/* Deferred positions are applied as they come; the handle is only a token. */
static void k_BeginDeferWindowPos(rc_cpu *c) { RC_RETURN(c, 0x00defe00u, 1); }
static void k_DeferWindowPos(rc_cpu *c)
{
    if (get(RC_ARG(c, 1))) {
        set_pos(c, RC_ARG(c, 1), RC_ARG(c, 2), (int32_t)RC_ARG(c, 3), (int32_t)RC_ARG(c, 4), (int32_t)RC_ARG(c, 5),
                (int32_t)RC_ARG(c, 6), RC_ARG(c, 7));
    }
    RC_RETURN(c, RC_ARG(c, 0), 8);
}
static void k_EndDeferWindowPos(rc_cpu *c) { RC_RETURN(c, 1, 1); }

void rc_window_resize(uint32_t hwnd, int32_t width, int32_t height)
{
    window *w = get(hwnd);
    if (!w) {
        return;
    }
    w->game = true; /* a display mode change: the window is the game's screen */
    w->rect = (rc_rect){0, 0, width, height};
    vis_gen++;
    w->crect = (rc_rect){0, 0, width, height};
    if (hwnd == main_hwnd) {
        rc_plat_window(width, height);
    }
}

/* WINDOWPLACEMENT: length, flags, showCmd, min, max (POINTs), normal (RECT). */
static void k_GetWindowPlacement(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    uint32_t p = RC_ARG(c, 1);
    if (!w) {
        RC_RETURN(c, 0, 2);
    }
    rc_st32(p + 4, 0);
    rc_st32(p + 8, (w->style & WS_MAXIMIZE) ? 3 : (w->style & WS_VISIBLE) ? 1 : 0);
    put_rect(p + 12, -1, -1, -1, -1);
    put_rect_r(p + 28, w->rect);
    RC_RETURN(c, 1, 2);
}

static void k_SetWindowPlacement(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), p = RC_ARG(c, 1);
    if (!get(hwnd)) {
        RC_RETURN(c, 0, 2);
    }
    rc_rect r = get_rect(p + 28);
    rc_wnd_move(c, hwnd, r.l, r.t, r.r - r.l, r.b - r.t, true);
    uint32_t cmd = rc_ld32(p + 8);
    if (cmd) {
        rc_wnd_show(c, hwnd, (int)cmd);
    }
    RC_RETURN(c, 1, 2);
}

/* ---- hit testing ---- */

static bool in(rc_rect r, int32_t x, int32_t y) { return x >= r.l && x < r.r && y >= r.t && y < r.b; }

static uint32_t child_at(uint32_t parent, int32_t x, int32_t y, bool enabled_only)
{
    uint32_t list[512];
    int n = siblings(parent, list, 512);
    for (int i = 0; i < n; i++) {
        window *w = get(list[i]);
        if (!(w->style & WS_VISIBLE) || !in(screen_area(w, true), x, y)) {
            continue;
        }
        if (enabled_only && (w->style & WS_DISABLED)) {
            return 0;
        }
        if (in(screen_area(w, false), x, y)) {
            uint32_t k = child_at(list[i], x, y, enabled_only);
            if (k) {
                return k;
            }
        }
        return list[i];
    }
    return 0;
}

uint32_t rc_wnd_from_point(int32_t x, int32_t y)
{
    uint32_t list[512];
    int n = siblings(0, list, 512);
    for (int i = 0; i < n; i++) {
        window *w = get(list[i]);
        if (!(w->style & WS_VISIBLE) || !in(screen_area(w, true), x, y)) {
            continue;
        }
        if (in(screen_area(w, false), x, y)) {
            uint32_t k = child_at(list[i], x, y, true);
            if (k) {
                return k;
            }
        }
        return list[i];
    }
    return 0;
}

static void k_WindowFromPoint(rc_cpu *c) { RC_RETURN(c, rc_wnd_from_point((int32_t)RC_ARG(c, 0), (int32_t)RC_ARG(c, 1)), 2); }

/* ChildWindowFromPoint(parent, POINT): client coordinates; the parent itself if no child. */
static void k_ChildWindowFromPoint(rc_cpu *c)
{
    uint32_t parent = RC_ARG(c, 0);
    window *w = get(parent);
    if (!w) {
        RC_RETURN(c, 0, 3);
    }
    int32_t ox, oy;
    rc_wnd_client_origin(parent, &ox, &oy);
    int32_t x = (int32_t)RC_ARG(c, 1) + ox, y = (int32_t)RC_ARG(c, 2) + oy;
    if (!in(screen_area(w, false), x, y)) {
        RC_RETURN(c, 0, 3);
    }
    uint32_t list[512];
    int n = siblings(parent, list, 512);
    for (int i = 0; i < n; i++) {
        if (in(screen_area(get(list[i]), true), x, y)) {
            RC_RETURN(c, list[i], 3);
        }
    }
    RC_RETURN(c, parent, 3);
}

/* ---- long values, properties ---- */

static uint32_t *long_slot(window *w, int32_t index)
{
    switch (index) {
    case -4: return &w->wndproc;
    case -6: return &w->instance;
    case -8: return &w->parent;
    case -12: return &w->id;
    case -16: return &w->style;
    case -20: return &w->exstyle;
    case -21: return &w->userdata;
    default: return NULL;
    }
}

uint32_t rc_wnd_long(uint32_t hwnd, int32_t index)
{
    window *w = get(hwnd);
    if (!w) {
        return 0;
    }
    uint32_t *s = long_slot(w, index), v = 0;
    if (s) {
        return *s;
    }
    if (index >= 0 && index + 4 <= EXTRA) {
        memcpy(&v, w->extra + index, 4);
    }
    return v;
}

void rc_wnd_set_long(uint32_t hwnd, int32_t index, uint32_t v)
{
    window *w = get(hwnd);
    if (w && index >= 0 && index + 4 <= EXTRA) {
        memcpy(w->extra + index, &v, 4);
    }
}

static void k_GetWindowLongA(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    int32_t index = (int32_t)RC_ARG(c, 1);
    if (!w) {
        RC_RETURN(c, 0, 2);
    }
    uint32_t *s = long_slot(w, index);
    if (s) {
        RC_RETURN(c, *s, 2);
    }
    if (index >= 0 && index + 4 <= EXTRA) {
        uint32_t v;
        memcpy(&v, w->extra + index, 4);
        RC_RETURN(c, v, 2);
    }
    rc_last_error = 1413; /* ERROR_INVALID_INDEX */
    RC_RETURN(c, 0, 2);
}

static void k_SetWindowLongA(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    int32_t index = (int32_t)RC_ARG(c, 1);
    uint32_t v = RC_ARG(c, 2), old = 0;
    if (!w) {
        RC_RETURN(c, 0, 3);
    }
    uint32_t *s = long_slot(w, index);
    if (s) {
        old = *s;
        *s = v;
        if (index == -16 || index == -20) {
            w->nc_dirty = true;
        }
        /* WS_VISIBLE set or cleared in the style shows or hides the window as on Windows (no
         * WM_SHOWWINDOW). Heroes IV sets WS_POPUP | WS_VISIBLE before its ShowWindow, which then
         * finds the window shown and changes nothing: the host window has to follow here. */
        if (index == -16 && ((old ^ v) & WS_VISIBLE)) {
            vis_gen++;
            if (!(v & WS_CHILD)) {
                invalidate_all();
                if (handle_of(w) == main_hwnd && !desktop_on) {
                    rc_plat_show((v & WS_VISIBLE) != 0);
                }
            }
        }
    } else if (index >= 0 && index + 4 <= EXTRA) {
        memcpy(&old, w->extra + index, 4);
        memcpy(w->extra + index, &v, 4);
    }
    RC_RETURN(c, old, 3);
}

static void k_GetWindowWord(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    int32_t index = (int32_t)RC_ARG(c, 1);
    uint16_t v = 0;
    if (w && index >= 0 && index + 2 <= EXTRA) {
        memcpy(&v, w->extra + index, 2);
    }
    RC_RETURN(c, v, 2);
}

static void k_SetWindowWord(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    int32_t index = (int32_t)RC_ARG(c, 1);
    uint16_t v = (uint16_t)RC_ARG(c, 2), old = 0;
    if (w && index >= 0 && index + 2 <= EXTRA) {
        memcpy(&old, w->extra + index, 2);
        memcpy(w->extra + index, &v, 2);
    }
    RC_RETURN(c, old, 3);
}

/* GetClassLongA(hwnd, index): GCL_MENUNAME -8, HBRBACKGROUND -10, HCURSOR -12, HICON -14,
 * HMODULE -16, CBWNDEXTRA -18, CBCLSEXTRA -20, WNDPROC -24, STYLE -26, ATOM -32, HICONSM -34. */
static uint32_t *class_slot(rc_wclass *k, int32_t index)
{
    switch (index) {
    case -8: return &k->menu_name;
    case -10: return &k->background;
    case -12: return &k->cursor;
    case -14: return &k->icon;
    case -16: return &k->instance;
    case -18: return &k->wnd_extra;
    case -20: return &k->cls_extra;
    case -24: return &k->wndproc;
    case -26: return &k->style;
    case -32: return &k->atom;
    case -34: return &k->icon_small;
    default: return NULL;
    }
}

static void k_GetClassLongA(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    int32_t index = (int32_t)RC_ARG(c, 1);
    uint32_t v = 0;
    if (w) {
        uint32_t *s = class_slot(w->cls, index);
        if (s) {
            v = *s;
        } else if (index >= 0 && index + 4 <= 64) {
            memcpy(&v, w->cls->cls_data + index, 4);
        }
    }
    RC_RETURN(c, v, 2);
}

static void k_SetClassLongA(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    int32_t index = (int32_t)RC_ARG(c, 1);
    uint32_t old = 0, v = RC_ARG(c, 2);
    if (w) {
        uint32_t *s = class_slot(w->cls, index);
        if (s) {
            old = *s;
            *s = v;
        } else if (index >= 0 && index + 4 <= 64) {
            memcpy(&old, w->cls->cls_data + index, 4);
            memcpy(w->cls->cls_data + index, &v, 4);
        }
    }
    RC_RETURN(c, old, 3);
}

/* Properties: by atom (below 0x10000) or by name. */
static int prop_find(window *w, uint32_t key)
{
    for (int i = 0; i < PROPS; i++) {
        if (!w->props[i].atom && !w->props[i].name[0]) {
            continue;
        }
        if (key < 0x10000 ? w->props[i].atom == key : !strcmp(w->props[i].name, rc_str(key))) {
            return i;
        }
    }
    return -1;
}

static void k_SetPropA(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    uint32_t key = RC_ARG(c, 1);
    if (!w) {
        RC_RETURN(c, 0, 3);
    }
    int i = prop_find(w, key);
    for (int k = 0; i < 0 && k < PROPS; k++) {
        if (!w->props[k].atom && !w->props[k].name[0]) {
            i = k;
        }
    }
    if (i < 0) {
        RC_RETURN(c, 0, 3);
    }
    if (key < 0x10000) {
        w->props[i].atom = key;
    } else {
        snprintf(w->props[i].name, sizeof w->props[i].name, "%s", rc_str(key));
    }
    w->props[i].value = RC_ARG(c, 2);
    RC_RETURN(c, 1, 3);
}

static void k_GetPropA(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    int i = w ? prop_find(w, RC_ARG(c, 1)) : -1;
    RC_RETURN(c, i >= 0 ? w->props[i].value : 0, 2);
}

static void k_RemovePropA(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    int i = w ? prop_find(w, RC_ARG(c, 1)) : -1;
    if (i < 0) {
        RC_RETURN(c, 0, 2);
    }
    uint32_t v = w->props[i].value;
    memset(&w->props[i], 0, sizeof w->props[i]);
    RC_RETURN(c, v, 2);
}

/* ---- painting calls ---- */

static void k_InvalidateRect(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), r = RC_ARG(c, 1);
    if (!hwnd || hwnd == DESKTOP_HWND) {
        invalidate_all();
        RC_RETURN(c, 1, 3);
    }
    if (get(hwnd)) {
        rc_rect x = r ? get_rect(r) : (rc_rect){0, 0, 0, 0};
        rc_wnd_invalidate(hwnd, r ? &x : NULL, RC_ARG(c, 2) != 0);
    }
    RC_RETURN(c, 1, 3);
}

static void k_InvalidateRgn(rc_cpu *c)
{
    gobj *o = rc_gobj_get(RC_ARG(c, 1), OB_REGION);
    rc_rect b = o ? rc_rgn_box(&o->rgn) : (rc_rect){0, 0, 0, 0};
    rc_wnd_invalidate(RC_ARG(c, 0), o ? &b : NULL, RC_ARG(c, 2) != 0);
    RC_RETURN(c, 1, 3);
}

static void k_ValidateRect(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    uint32_t r = RC_ARG(c, 1);
    if (w) {
        rc_rect x = r ? get_rect(r) : (rc_rect){-0x40000000, -0x40000000, 0x40000000, 0x40000000};
        if (x.l <= w->upd.l && x.t <= w->upd.t && x.r >= w->upd.r && x.b >= w->upd.b) {
            w->upd = (rc_rect){0, 0, 0, 0};
        }
    }
    RC_RETURN(c, 1, 2);
}

static void k_ValidateRgn(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    if (w) {
        w->upd = (rc_rect){0, 0, 0, 0};
    }
    RC_RETURN(c, 1, 2);
}

static void k_GetUpdateRect(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    if (!w) {
        RC_RETURN(c, 0, 3);
    }
    if (RC_ARG(c, 1)) {
        put_rect_r(RC_ARG(c, 1), w->upd);
    }
    RC_RETURN(c, !rc_rect_empty(w->upd), 3);
}

static void k_GetUpdateRgn(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    gobj *o = rc_gobj_get(RC_ARG(c, 1), OB_REGION);
    if (!w || !o) {
        RC_RETURN(c, 0, 3);
    }
    rc_rgn_set(&o->rgn, w->upd.l, w->upd.t, w->upd.r, w->upd.b);
    RC_RETURN(c, rc_rect_empty(w->upd) ? 1 : 2, 3);
}

/* ExcludeUpdateRgn(dc, hwnd): the window's update area is left out of the DC's clip region. */
static void k_ExcludeUpdateRgn(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    window *w = get(RC_ARG(c, 1));
    if (d && w && !rc_rect_empty(w->upd)) {
        if (!d->has_clip) {
            rc_rgn_set(&d->clip, -0x40000000, -0x40000000, 0x40000000, 0x40000000);
            d->has_clip = true;
        }
        rc_rgn_cut_rect(&d->clip, w->upd);
    }
    RC_RETURN(c, 2, 2);
}

static void k_UpdateWindow(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0);
    if (get(hwnd)) {
        rc_wnd_update(c, hwnd);
    }
    RC_RETURN(c, get(hwnd) != NULL, 1);
}

/* RedrawWindow(hwnd, rect, region, flags) */
static void k_RedrawWindow(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), r = RC_ARG(c, 1), flags = RC_ARG(c, 3);
    if (!hwnd || hwnd == DESKTOP_HWND) {
        if (flags & 1) {
            invalidate_all();
        }
        RC_RETURN(c, 1, 4);
    }
    window *w = get(hwnd);
    if (!w) {
        RC_RETURN(c, 0, 4);
    }
    if (flags & 0x1) { /* RDW_INVALIDATE */
        rc_rect x = r ? get_rect(r) : (rc_rect){0, 0, 0, 0};
        rc_wnd_invalidate(hwnd, r ? &x : NULL, (flags & 0x4) != 0);
        if (flags & 0x400) { /* RDW_FRAME */
            w->nc_dirty = true;
        }
    }
    if (flags & 0x8) { /* RDW_VALIDATE */
        w->upd = (rc_rect){0, 0, 0, 0};
    }
    if (flags & 0x300) { /* RDW_UPDATENOW, RDW_ERASENOW */
        rc_wnd_update(c, hwnd);
    }
    RC_RETURN(c, 1, 4);
}

static void k_LockWindowUpdate(rc_cpu *c) { RC_RETURN(c, 1, 1); }

/* PAINTSTRUCT: hdc, fErase, rcPaint, ... (64 bytes). */
static void k_BeginPaint(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), ps = RC_ARG(c, 1);
    window *w = get(hwnd);
    if (!w) {
        RC_RETURN(c, 0, 2);
    }
    uint32_t hdc = rc_wnd_dc(hwnd, false);
    rc_rect upd = rc_rect_empty(w->upd) ? (rc_rect){0, 0, 0, 0} : w->upd;
    bool erase = w->erase;
    w->upd = (rc_rect){0, 0, 0, 0};
    w->erase = false;
    rc_dc *d = rc_dc_get(hdc);
    if (d) {
        d->vis_kind = 0; /* a paint DC keeps the update region's limit */
    }
    if (d && !rc_rect_empty(upd)) {
        /* drawing is limited to what was invalid */
        rc_rgn_and_rect(&d->vis, (rc_rect){upd.l + d->org_x, upd.t + d->org_y, upd.r + d->org_x, upd.b + d->org_y});
    }
    uint32_t erased = erase ? send(c, hwnd, WM_ERASEBKGND, hdc, 0) : 0;
    memset(rc_ptr(ps), 0, 64);
    rc_st32(ps, hdc);
    rc_st32(ps + 4, erase && !erased);
    put_rect_r(ps + 8, upd);
    RC_RETURN(c, hdc, 2);
}

static void k_EndPaint(rc_cpu *c)
{
    rc_wnd_release_dc(rc_ld32(RC_ARG(c, 1)));
    RC_RETURN(c, 1, 2);
}

static void k_GetDC(rc_cpu *c) { RC_RETURN(c, rc_wnd_dc(RC_ARG(c, 0) == DESKTOP_HWND ? 0 : RC_ARG(c, 0), false), 1); }
static void k_GetWindowDC(rc_cpu *c)
{
    RC_RETURN(c, rc_wnd_dc(RC_ARG(c, 0) == DESKTOP_HWND ? 0 : RC_ARG(c, 0), true), 1);
}
/* GetDCEx(hwnd, clip region, flags): DCX_WINDOW 1 */
static void k_GetDCEx(rc_cpu *c)
{
    RC_RETURN(c, rc_wnd_dc(RC_ARG(c, 0) == DESKTOP_HWND ? 0 : RC_ARG(c, 0), (RC_ARG(c, 2) & 1) != 0), 3);
}
static void k_ReleaseDC(rc_cpu *c)
{
    rc_wnd_release_dc(RC_ARG(c, 1));
    RC_RETURN(c, 1, 2);
}
static void k_WindowFromDC(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    RC_RETURN(c, d && d->hwnd != 0xffffffffu ? d->hwnd : 0, 1);
}

/* ---- DefWindowProc and the frame ---- */

/* The parts of a window's frame, in screen coordinates. */
typedef struct frame_parts {
    rc_rect window, caption, close, menu, vscroll, hscroll, client;
} frame_parts;

static void parts_of(window *w, frame_parts *p)
{
    memset(p, 0, sizeof *p);
    rc_rect r = screen_area(w, true);
    p->window = r;
    p->client = screen_area(w, false);
    int32_t b = border_of(w), cap = caption_of(w);
    if (cap) {
        p->caption = (rc_rect){r.l + b, r.t + b, r.r - b, r.t + b + cap - 1};
        if (w->style & WS_SYSMENU) {
            int32_t s = cap - 5;
            p->close = (rc_rect){p->caption.r - s - 4, p->caption.t + 2, p->caption.r - 2, p->caption.t + 2 + s};
        }
    }
    int32_t mh = !(w->style & WS_CHILD) && w->menu ? rc_menu_bar_height(handle_of(w)) : 0;
    if (mh) {
        p->menu = (rc_rect){r.l + b, r.t + b + cap, r.r - b, r.t + b + cap + mh};
    }
    if (w->style & WS_VSCROLL) {
        p->vscroll = (rc_rect){p->client.r, p->client.t, p->client.r + 16, p->client.b};
    }
    if (w->style & WS_HSCROLL) {
        p->hscroll = (rc_rect){p->client.l, p->client.b, p->client.r, p->client.b + 16};
    }
}

static uint32_t caption_font(void)
{
    static uint32_t f;
    if (!f) {
        f = rc_gobj_new(OB_FONT);
        gobj *o = rc_gobj_get(f, OB_FONT);
        snprintf(o->face, sizeof o->face, "MS Sans Serif");
        o->height = -11;
        o->weight = 700;
        o->stock = true;
    }
    return f;
}

static void nc_paint(window *w)
{
    if (w->game || !rc_desktop_active()) {
        return;
    }
    uint32_t hwnd = handle_of(w), hdc = rc_wnd_dc(hwnd, true);
    rc_dc *d = rc_dc_get(hdc);
    if (!d) {
        return;
    }
    frame_parts p;
    parts_of(w, &p);
    int32_t ox = p.window.l, oy = p.window.t;
#define LOCAL(R) ((rc_rect){(R).l - ox, (R).t - oy, (R).r - ox, (R).b - oy})
    rc_rect whole = LOCAL(p.window);
    int32_t b = border_of(w);
    uint32_t face = rc_sys_color(15);
    if (b >= 3) {
        rc_dc_fill(d, (rc_rect){whole.l, whole.t, whole.r, whole.t + b}, face);
        rc_dc_fill(d, (rc_rect){whole.l, whole.b - b, whole.r, whole.b}, face);
        rc_dc_fill(d, (rc_rect){whole.l, whole.t, whole.l + b, whole.b}, face);
        rc_dc_fill(d, (rc_rect){whole.r - b, whole.t, whole.r, whole.b}, face);
        rc_draw_edge(d, whole, 0x5 /* EDGE_RAISED */, 0xf);
    } else if (b == 1) {
        uint32_t k = rc_sys_color(6);
        rc_dc_fill(d, (rc_rect){whole.l, whole.t, whole.r, whole.t + 1}, k);
        rc_dc_fill(d, (rc_rect){whole.l, whole.b - 1, whole.r, whole.b}, k);
        rc_dc_fill(d, (rc_rect){whole.l, whole.t, whole.l + 1, whole.b}, k);
        rc_dc_fill(d, (rc_rect){whole.r - 1, whole.t, whole.r, whole.b}, k);
    }
    if (!rc_rect_empty(p.caption)) {
        bool active = rc_wnd_top_level(hwnd) == active_hwnd;
        rc_rect cap = LOCAL(p.caption);
        rc_dc_fill(d, cap, rc_sys_color(active ? 2 : 3));
        rc_dc_fill(d, (rc_rect){cap.l, cap.b, cap.r, cap.b + 1}, face);
        uint32_t old_font = d->font;
        d->font = caption_font();
        d->text_color = rc_sys_color(active ? 9 : 19);
        d->bk_mode = 1;
        rc_rect tr = {cap.l + 4, cap.t, cap.r - 24, cap.b};
        rc_draw_text(d, w->text ? w->text : "", -1, &tr, 0x24 | 0x8000); /* VCENTER SINGLELINE END_ELLIPSIS */
        d->font = old_font;
        if (!rc_rect_empty(p.close)) {
            rc_rect cl = LOCAL(p.close);
            rc_dc_fill(d, cl, face);
            rc_draw_edge(d, cl, 0x5, 0xf);
            int32_t s = cl.b - cl.t - 7, x0 = cl.l + (cl.r - cl.l - s) / 2, y0 = cl.t + 3;
            uint32_t black = rc_dc_pixel(d, 0);
            for (int32_t k = 0; k <= s; k++) {
                rc_dc_plot(d, x0 + k, y0 + k, black);
                rc_dc_plot(d, x0 + k + 1, y0 + k, black);
                rc_dc_plot(d, x0 + s - k, y0 + k, black);
                rc_dc_plot(d, x0 + s - k + 1, y0 + k, black);
            }
        }
    }
    if (!rc_rect_empty(p.menu)) {
        rc_menu_draw_bar(d, hwnd, LOCAL(p.menu));
    }
    rc_rect client = LOCAL(p.client);
    rc_rect e = client;
    if (w->style & WS_VSCROLL) {
        e.r += 16;
    }
    if (w->style & WS_HSCROLL) {
        e.b += 16;
    }
    if (w->exstyle & WS_EX_CLIENTEDGE) {
        rc_draw_edge(d, (rc_rect){e.l - 2, e.t - 2, e.r + 2, e.b + 2}, 0xa /* EDGE_SUNKEN */, 0xf);
    } else if (w->exstyle & WS_EX_STATICEDGE) {
        rc_draw_edge(d, (rc_rect){e.l - 1, e.t - 1, e.r + 1, e.b + 1}, 0x2 /* BDR_SUNKENOUTER */, 0xf);
    }
    if (!rc_rect_empty(p.vscroll)) {
        rc_scroll_draw(d, LOCAL(p.vscroll), true, &w->sb[1], !(w->style & WS_DISABLED));
    }
    if (!rc_rect_empty(p.hscroll)) {
        rc_scroll_draw(d, LOCAL(p.hscroll), false, &w->sb[0], !(w->style & WS_DISABLED));
    }
    if (!rc_rect_empty(p.vscroll) && !rc_rect_empty(p.hscroll)) {
        rc_dc_fill(d, (rc_rect){client.r, client.b, client.r + 16, client.b + 16}, face);
    }
#undef LOCAL
    rc_wnd_release_dc(hdc);
}

static uint32_t nc_hit(window *w, int32_t x, int32_t y)
{
    frame_parts p;
    parts_of(w, &p);
    if (!in(p.window, x, y)) {
        return HTNOWHERE;
    }
    if (in(p.client, x, y)) {
        return HTCLIENT;
    }
    if (in(p.close, x, y)) {
        return HTCLOSE;
    }
    if (in(p.caption, x, y)) {
        return HTCAPTION;
    }
    if (in(p.menu, x, y)) {
        return HTMENU;
    }
    if (in(p.vscroll, x, y)) {
        return HTVSCROLL;
    }
    if (in(p.hscroll, x, y)) {
        return HTHSCROLL;
    }
    return HTBORDER;
}

/* Dragging a window by its caption. */
static void move_loop(rc_cpu *c, window *w, int32_t x0, int32_t y0)
{
    uint32_t hwnd = handle_of(w);
    rc_rect start = w->rect;
    int32_t x = x0, y = y0;
    while (rc_input_track(c, &x, &y)) {
        if (get(hwnd) != w) {
            return;
        }
        rc_wnd_move(c, hwnd, start.l + x - x0, start.t + y - y0, start.r - start.l, start.b - start.t, true);
        rc_wnd_paint_all(c);
        rc_desktop_present(false);
    }
}

static uint32_t def_window_proc(rc_cpu *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    window *w = get(hwnd);
    switch (msg) {
    case WM_NCCREATE:
        if (w && lp && rc_ld32(lp + 36) >= 0x10000) {
            rc_wnd_set_text(hwnd, rc_str(rc_ld32(lp + 36)));
        }
        return 1;
    case WM_NCCALCSIZE:
        if (w && lp) {
            put_rect_r(lp, default_client(w, get_rect(lp)));
        }
        return 0;
    case WM_NCPAINT:
        if (w) {
            nc_paint(w);
        }
        return 0;
    case WM_NCACTIVATE:
        if (w) {
            w->nc_dirty = true;
        }
        return 1;
    case WM_NCHITTEST:
        return w ? nc_hit(w, (int16_t)(lp & 0xffff), (int16_t)(lp >> 16)) : HTNOWHERE;
    case WM_NCLBUTTONDOWN: {
        if (!w) {
            return 0;
        }
        int32_t x = (int16_t)(lp & 0xffff), y = (int16_t)(lp >> 16);
        rc_wnd_activate(c, hwnd);
        if (get(hwnd) != w) {
            return 0;
        }
        if (wp == HTCAPTION && !(w->style & (WS_MAXIMIZE | WS_CHILD))) {
            move_loop(c, w, x, y);
        } else if (wp == HTCLOSE) {
            rc_post_message(hwnd, WM_SYSCOMMAND, 0xf060u /* SC_CLOSE */, lp);
        } else if (wp == HTMENU) {
            rc_menu_bar_track(c, hwnd, x, y, 0);
        } else if (wp == HTVSCROLL || wp == HTHSCROLL) {
            frame_parts p;
            parts_of(w, &p);
            bool v = wp == HTVSCROLL;
            rc_scroll_track(c, hwnd, 0, v ? p.vscroll : p.hscroll, v, &w->sb[v ? 1 : 0], x, y);
        }
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0u) == 0xf060u) { /* SC_CLOSE */
            rc_post_message(hwnd, WM_CLOSE, 0, 0);
        } else if ((wp & 0xfff0u) == 0xf100u) { /* SC_KEYMENU */
            uint32_t top = rc_wnd_top_level(hwnd);
            if (rc_wnd_menu(top)) {
                rc_menu_bar_track(c, top, -1, -1, lp);
            }
        }
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == 0x73 && (lp & (1u << 29))) { /* Alt+F4 */
            rc_post_message(rc_wnd_top_level(hwnd), WM_SYSCOMMAND, 0xf060u, 0);
        }
        return 0;
    case WM_SYSKEYUP:
        if (wp == 0x12 || wp == 0x79) { /* Alt or F10 alone: the menu bar */
            uint32_t top = rc_wnd_top_level(hwnd);
            if (rc_wnd_menu(top)) {
                rc_menu_bar_track(c, top, -1, -1, 0);
            }
        }
        return 0;
    case WM_SYSCHAR:
        if (lp & (1u << 29)) { /* Alt+letter: that menu */
            uint32_t top = rc_wnd_top_level(hwnd);
            if (rc_wnd_menu(top)) {
                rc_menu_bar_track(c, top, -1, -1, wp);
            }
        }
        return 0;
    case WM_CLOSE:
        rc_wnd_destroy(c, hwnd);
        return 0;
    case WM_ERASEBKGND: {
        uint32_t color;
        rc_dc *d = rc_dc_get(wp);
        if (w && d && rc_brush_color(w->cls->background, &color)) {
            rc_dc_fill(d, rc_wnd_client(hwnd), color);
            return 1;
        }
        return 0;
    }
    case WM_PAINT:
        if (w) {
            if (w->erase) {
                uint32_t hdc = rc_wnd_dc(hwnd, false);
                send(c, hwnd, WM_ERASEBKGND, hdc, 0);
                rc_wnd_release_dc(hdc);
            }
            if (get(hwnd) == w) {
                w->upd = (rc_rect){0, 0, 0, 0};
                w->erase = false;
            }
        }
        return 0;
    case WM_SETTEXT:
        rc_wnd_set_text(hwnd, lp ? rc_str(lp) : "");
        if (w && caption_of(w)) {
            w->nc_dirty = true;
        }
        if (hwnd == main_hwnd) {
            rc_plat_title(rc_wnd_text(hwnd));
        }
        return 1;
    case WM_GETTEXT:
        return wp ? rc_put_str(lp, wp, rc_wnd_text(hwnd)) : 0;
    case WM_GETTEXTLENGTH:
        return (uint32_t)strlen(rc_wnd_text(hwnd));
    case WM_SETCURSOR: {
        uint32_t hit = lp & 0xffff, parent = rc_wnd_parent(hwnd);
        if (parent && send(c, parent, WM_SETCURSOR, wp, lp)) {
            return 1;
        }
        if (hit == HTCLIENT && w && w->cls->cursor) {
            rc_set_cursor(w->cls->cursor);
            return 1;
        }
        rc_set_cursor(rc_arrow_cursor());
        return hit != HTCLIENT;
    }
    case WM_MOUSEACTIVATE: {
        uint32_t parent = rc_wnd_parent(hwnd);
        uint32_t r = parent ? send(c, parent, WM_MOUSEACTIVATE, wp, lp) : 0;
        return r ? r : 1; /* MA_ACTIVATE */
    }
    case WM_ACTIVATE:
        if ((wp & 0xffff) && w && !(w->style & WS_MINIMIZE)) {
            if (!focus_hwnd || rc_wnd_top_level(focus_hwnd) != hwnd) {
                rc_wnd_set_focus(c, hwnd);
            }
        }
        return 0;
    case WM_WINDOWPOSCHANGED:
        if (w && lp) {
            uint32_t flags = rc_ld32(lp + 24);
            if (!(flags & SWP_NOMOVE) || (flags & SWP_FRAMECHANGED)) {
                send(c, hwnd, WM_MOVE, 0, mkpoint(w->rect.l + w->crect.l, w->rect.t + w->crect.t));
            }
            if (get(hwnd) == w && (!(flags & SWP_NOSIZE) || (flags & SWP_FRAMECHANGED))) {
                send(c, hwnd, WM_SIZE, (w->style & WS_MAXIMIZE) ? 2 : 0,
                     mkpoint(w->crect.r - w->crect.l, w->crect.b - w->crect.t));
            }
        }
        return 0;
    case WM_SETREDRAW:
        if (w) {
            w->redraw_off = wp == 0;
            if (wp) {
                invalidate_frame(w);
            }
        }
        return 0;
    case WM_SETFONT:
        if (w) {
            w->font = wp;
        }
        return 0;
    case WM_GETFONT:
        return w ? w->font : 0;
    case WM_CONTEXTMENU: {
        uint32_t parent = rc_wnd_parent(hwnd);
        if (parent) {
            send(c, parent, WM_CONTEXTMENU, wp, lp);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        int32_t ox, oy;
        rc_wnd_client_origin(hwnd, &ox, &oy);
        send(c, hwnd, WM_CONTEXTMENU, hwnd, mkpoint((int16_t)(lp & 0xffff) + ox, (int16_t)(lp >> 16) + oy));
        return 0;
    }
    default:
        if (msg >= WM_CTLCOLORMSGBOX && msg <= WM_CTLCOLORSTATIC) {
            rc_dc *d = rc_dc_get(wp);
            bool white = msg == 0x0133u || msg == 0x0134u; /* EDIT, LISTBOX */
            if (d) {
                d->text_color = rc_sys_color(8);
                d->bk_color = rc_sys_color(white ? 5 : 15);
            }
            return rc_sys_brush(white ? 5 : 15);
        }
        return 0;
    }
}

static void k_DefWindowProcA(rc_cpu *c)
{
    RC_RETURN(c, def_window_proc(c, RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3)), 4);
}

uint32_t rc_def_window_proc(rc_cpu *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    return def_window_proc(c, hwnd, msg, wp, lp);
}

/* CallWindowProcA(proc, hwnd, msg, wp, lp) */
static void k_CallWindowProcA(rc_cpu *c)
{
    uint32_t proc = RC_ARG(c, 0);
    uint32_t args[4] = {RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3), RC_ARG(c, 4)};
    RC_RETURN(c, proc ? rc_call_guest(c, proc, 4, args) : 0, 5);
}

/* ---- menus on windows (the bar itself is menu.c's) ---- */

void rc_wnd_set_menu(uint32_t hwnd, uint32_t menu)
{
    window *w = get(hwnd);
    if (w && !(w->style & WS_CHILD)) {
        w->menu = menu;
    }
}

static void k_SetMenu(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0);
    window *w = get(hwnd);
    if (!w || (w->style & WS_CHILD)) {
        RC_RETURN(c, 0, 2);
    }
    w->menu = RC_ARG(c, 1);
    set_pos(c, hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RC_RETURN(c, 1, 2);
}

static void k_GetMenu(rc_cpu *c) { RC_RETURN(c, rc_wnd_menu(RC_ARG(c, 0)), 1); }

static void k_DrawMenuBar(rc_cpu *c)
{
    window *w = get(RC_ARG(c, 0));
    if (w) {
        w->nc_dirty = true;
    }
    RC_RETURN(c, 1, 1);
}

/* ---- metrics ---- */

static void screen_size(int *w, int *h)
{
    if (desktop_on) {
        int32_t a, b;
        rc_desktop_size(&a, &b);
        *w = a;
        *h = b;
        return;
    }
    rc_plat_area(w, h);
    if (*w <= 0 || *h <= 0) {
        *w = 1024;
        *h = 768;
    }
}

static void k_GetSystemMetrics(rc_cpu *c)
{
    int w, h;
    screen_size(&w, &h);
    uint32_t v = 0;
    switch (RC_ARG(c, 0)) {
    case 0: case 16: case 61: case 78: v = (uint32_t)w; break;      /* CXSCREEN, FULLSCREEN, MAXIMIZED */
    case 1: case 62: case 79: v = (uint32_t)h; break;
    case 17: v = (uint32_t)h - 19; break;                             /* CYFULLSCREEN */
    case 2: case 3: case 9: case 10: case 20: case 21: v = 16; break; /* scroll bars and thumbs */
    case 4: v = 19; break;                                            /* CYCAPTION */
    case 5: case 6: v = 1; break;                                     /* CXBORDER */
    case 7: case 8: v = 3; break;                                     /* CXDLGFRAME */
    case 11: case 12: case 13: case 14: v = 32; break;                /* icons, cursors */
    case 15: v = 19; break;                                           /* CYMENU */
    case 19: v = 1; break;                                            /* MOUSEPRESENT */
    case 23: v = 0; break;                                            /* SWAPBUTTON */
    case 28: case 34: v = 112; break;                                 /* CXMIN, CXMINTRACK */
    case 29: case 35: v = 27; break;
    case 30: case 31: v = 18; break;                                  /* CXSIZE */
    case 32: case 33: v = 4; break;                                   /* CXFRAME */
    case 36: case 37: case 68: case 69: v = 4; break;                 /* double click, drag */
    case 38: case 39: v = 75; break;
    case 43: v = 3; break;                                            /* CMOUSEBUTTONS */
    case 45: case 46: v = 2; break;                                   /* CXEDGE */
    case 49: case 50: v = 16; break;                                  /* small icons */
    case 51: v = 15; break;                                           /* CYSMCAPTION */
    case 52: case 53: v = 12; break;
    case 54: case 55: v = 18; break;                                  /* menu buttons */
    case 71: case 72: v = 13; break;                                  /* menu check marks */
    case 75: case 80: case 83: case 84: v = 1; break;                 /* wheel, monitors, focus border */
    default: break;
    }
    RC_RETURN(c, v, 1);
}

static void put_logfont(uint32_t p, int32_t height, uint32_t weight)
{
    memset(rc_ptr(p), 0, 60);
    rc_st32(p, (uint32_t)height);
    rc_st32(p + 16, weight);
    rc_put_str(p + 28, 32, "MS Sans Serif");
}

static void k_SystemParametersInfoA(rc_cpu *c)
{
    uint32_t action = RC_ARG(c, 0), pv = RC_ARG(c, 2);
    switch (action) {
    case 0x30: { /* SPI_GETWORKAREA */
        int w, h;
        screen_size(&w, &h);
        if (pv) {
            put_rect(pv, 0, 0, w, h);
        }
        break;
    }
    case 0x10: case 0x1f: case 0x0a: /* screen saver off, keyboard delay and speed 0 */
        if (pv) {
            rc_st32(pv, 0);
        }
        break;
    case 0x29: /* SPI_GETNONCLIENTMETRICS: sizes and the fonts (MS Sans Serif 8) */
        if (pv) {
            uint32_t n = rc_ld32(pv);
            memset(rc_ptr(pv + 4), 0, n > 4 ? n - 4 : 0);
            static const uint32_t ints[5][2] = {{4, 1}, {8, 16}, {12, 16}, {16, 18}, {20, 18}};
            for (int i = 0; i < 5; i++) {
                rc_st32(pv + ints[i][0], ints[i][1]);
            }
            static const uint32_t fonts[5] = {24, 92, 160, 220, 280};
            for (int f = 0; f < 5; f++) {
                if (fonts[f] + 60 <= n) {
                    put_logfont(pv + fonts[f], -11, f == 0 ? 700 : 400);
                }
            }
            if (n >= 92) {
                rc_st32(pv + 84, 15);
                rc_st32(pv + 88, 15);
            }
            if (n >= 160) {
                rc_st32(pv + 152, 18);
                rc_st32(pv + 156, 19);
            }
        }
        break;
    case 0x1e: /* SPI_GETICONTITLELOGFONT */
        if (pv) {
            put_logfont(pv, -11, 400);
        }
        break;
    default:
        break;
    }
    RC_RETURN(c, 1, 4);
}

/* One monitor: the desktop. */
#define MONITOR 0x00c0ffeeu
static void k_MonitorFromWindow(rc_cpu *c) { RC_RETURN(c, MONITOR, 2); }
static void k_MonitorFromPoint(rc_cpu *c) { RC_RETURN(c, MONITOR, 3); }
static void k_MonitorFromRect(rc_cpu *c) { RC_RETURN(c, MONITOR, 2); }

/* GetMonitorInfoA(monitor, MONITORINFO: size, monitor, work area, flags) */
static void k_GetMonitorInfoA(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 1);
    int w, h;
    screen_size(&w, &h);
    put_rect(p + 4, 0, 0, w, h);
    put_rect(p + 20, 0, 0, w, h);
    rc_st32(p + 36, 1); /* MONITORINFOF_PRIMARY */
    if (rc_ld32(p) >= 72) {
        rc_put_str(p + 40, 32, "DISPLAY1");
    }
    RC_RETURN(c, 1, 2);
}


/* GetWindowThreadProcessId(hwnd, pid): the window's thread; the process is always 1. */
static void k_GetWindowThreadProcessId(rc_cpu *c)
{
    uint32_t pid = RC_ARG(c, 1);
    if (pid) {
        rc_st32(pid, 1);
    }
    RC_RETURN(c, rc_wnd_thread(RC_ARG(c, 0)), 2);
}

/* ChangeDisplaySettingsA(mode, flags): the display is the window; any mode is fine. */
static void k_ChangeDisplaySettingsA(rc_cpu *c) { RC_RETURN(c, 0, 2); } /* DISP_CHANGE_SUCCESSFUL */

const rc_import_def rc_user32_windows[] = {
    {U32, "MonitorFromWindow", k_MonitorFromWindow},
    {U32, "MonitorFromPoint", k_MonitorFromPoint},
    {U32, "MonitorFromRect", k_MonitorFromRect},
    {U32, "GetMonitorInfoA", k_GetMonitorInfoA},
    {U32, "CreateWindowExA", k_CreateWindowExA},
    {U32, "DestroyWindow", k_DestroyWindow},
    {U32, "ShowWindow", k_ShowWindow},
    {U32, "ShowOwnedPopups", k_ShowOwnedPopups},
    {U32, "EnableWindow", k_EnableWindow},
    {U32, "IsWindow", k_IsWindow},
    {U32, "IsWindowVisible", k_IsWindowVisible},
    {U32, "IsWindowEnabled", k_IsWindowEnabled},
    {U32, "IsIconic", k_IsIconic},
    {U32, "IsZoomed", k_IsZoomed},
    {U32, "IsWindowUnicode", k_IsWindowUnicode},
    {U32, "SetFocus", k_SetFocus},
    {U32, "GetFocus", k_GetFocus},
    {U32, "GetActiveWindow", k_GetActiveWindow},
    {U32, "SetActiveWindow", k_SetActiveWindow},
    {U32, "GetForegroundWindow", k_GetForegroundWindow},
    {U32, "SetForegroundWindow", k_SetForegroundWindow},
    {U32, "BringWindowToTop", k_BringWindowToTop},
    {U32, "GetCapture", k_GetCapture},
    {U32, "SetCapture", k_SetCapture},
    {U32, "ReleaseCapture", k_ReleaseCapture},
    {U32, "GetLastActivePopup", k_GetLastActivePopup},
    {U32, "GetParent", k_GetParent},
    {U32, "SetParent", k_SetParent},
    {U32, "GetWindow", k_GetWindow},
    {U32, "GetTopWindow", k_GetTopWindow},
    {U32, "GetDesktopWindow", k_GetDesktopWindow},
    {U32, "IsChild", k_IsChild},
    {U32, "GetDlgCtrlID", k_GetDlgCtrlID},
    {U32, "GetClassNameA", k_GetClassNameA},
    {U32, "FindWindowA", k_FindWindowA},
    {U32, "EnumChildWindows", k_EnumChildWindows},
    {U32, "EnumWindows", k_EnumWindows},
    {U32, "SetWindowTextA", k_SetWindowTextA},
    {U32, "GetWindowTextA", k_GetWindowTextA},
    {U32, "GetWindowTextLengthA", k_GetWindowTextLengthA},
    {U32, "GetClientRect", k_GetClientRect},
    {U32, "GetWindowRect", k_GetWindowRect},
    {U32, "ClientToScreen", k_ClientToScreen},
    {U32, "ScreenToClient", k_ScreenToClient},
    {U32, "MapWindowPoints", k_MapWindowPoints},
    {U32, "SetWindowPos", k_SetWindowPos},
    {U32, "MoveWindow", k_MoveWindow},
    {U32, "BeginDeferWindowPos", k_BeginDeferWindowPos},
    {U32, "DeferWindowPos", k_DeferWindowPos},
    {U32, "EndDeferWindowPos", k_EndDeferWindowPos},
    {U32, "GetWindowPlacement", k_GetWindowPlacement},
    {U32, "SetWindowPlacement", k_SetWindowPlacement},
    {U32, "WindowFromPoint", k_WindowFromPoint},
    {U32, "ChildWindowFromPoint", k_ChildWindowFromPoint},
    {U32, "GetWindowLongA", k_GetWindowLongA},
    {U32, "SetWindowLongA", k_SetWindowLongA},
    {U32, "GetWindowWord", k_GetWindowWord},
    {U32, "SetWindowWord", k_SetWindowWord},
    {U32, "GetClassLongA", k_GetClassLongA},
    {U32, "SetClassLongA", k_SetClassLongA},
    {U32, "SetPropA", k_SetPropA},
    {U32, "GetPropA", k_GetPropA},
    {U32, "RemovePropA", k_RemovePropA},
    {U32, "InvalidateRect", k_InvalidateRect},
    {U32, "InvalidateRgn", k_InvalidateRgn},
    {U32, "ValidateRect", k_ValidateRect},
    {U32, "ValidateRgn", k_ValidateRgn},
    {U32, "GetUpdateRect", k_GetUpdateRect},
    {U32, "GetUpdateRgn", k_GetUpdateRgn},
    {U32, "ExcludeUpdateRgn", k_ExcludeUpdateRgn},
    {U32, "UpdateWindow", k_UpdateWindow},
    {U32, "RedrawWindow", k_RedrawWindow},
    {U32, "LockWindowUpdate", k_LockWindowUpdate},
    {U32, "BeginPaint", k_BeginPaint},
    {U32, "EndPaint", k_EndPaint},
    {U32, "GetDC", k_GetDC},
    {U32, "GetWindowDC", k_GetWindowDC},
    {U32, "GetDCEx", k_GetDCEx},
    {U32, "ReleaseDC", k_ReleaseDC},
    {U32, "WindowFromDC", k_WindowFromDC},
    {U32, "DefWindowProcA", k_DefWindowProcA},
    {U32, "CallWindowProcA", k_CallWindowProcA},
    {U32, "SetMenu", k_SetMenu},
    {U32, "GetMenu", k_GetMenu},
    {U32, "DrawMenuBar", k_DrawMenuBar},
    {U32, "GetSystemMetrics", k_GetSystemMetrics},
    {U32, "GetWindowThreadProcessId", k_GetWindowThreadProcessId},
    {U32, "ChangeDisplaySettingsA", k_ChangeDisplaySettingsA},
    {U32, "SystemParametersInfoA", k_SystemParametersInfoA},
};
const uint32_t rc_user32_windows_count = sizeof rc_user32_windows / sizeof rc_user32_windows[0];
