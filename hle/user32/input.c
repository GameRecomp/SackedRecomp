/* SPDX-License-Identifier: MIT */
/*
 * user32: the message queue and input. Host input becomes the messages Windows would post
 * (mouse, keys, activation); key state is kept for GetKeyboardState and ToAscii, with a US
 * keyboard layout.
 *
 * In a game (shown through DirectDraw) input goes straight to the main window (or the window
 * with the capture, or the focus for keys). On our desktop, input is turned into messages when
 * the program reads them: the window under the pointer is found and asked (WM_NCHITTEST,
 * WM_SETCURSOR, WM_MOUSEACTIVATE), which needs a guest CPU. Timers, hooks, carets and the
 * modal loops (dialogs, menus, dragging) are here too.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <string.h>

#include "rc/hle.h"
#include "rc/hle_modules.h"
#include "rc/log.h"
#include "rc/platform.h"
#include "rc/process.h"
#include "rc/thread.h"
#include "rc/vm.h"
#include "user32.h"

#define U32 "USER32.dll"

#define WM_PAINT 0x000fu
#define WM_CLOSE 0x0010u
#define WM_QUIT 0x0012u
#define WM_ACTIVATEAPP 0x001cu
#define WM_SETCURSOR 0x0020u
#define WM_MOUSEACTIVATE 0x0021u
#define WM_NCHITTEST 0x0084u
#define WM_NCMOUSEMOVE 0x00a0u
#define WM_NCLBUTTONDOWN 0x00a1u
#define WM_KEYDOWN 0x0100u
#define WM_KEYUP 0x0101u
#define WM_CHAR 0x0102u
#define WM_SYSKEYDOWN 0x0104u
#define WM_SYSKEYUP 0x0105u
#define WM_SYSCHAR 0x0106u
#define WM_TIMER 0x0113u
#define WM_MOUSEMOVE 0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_MOUSEWHEEL 0x020au
#define HTTRANSPARENT 0xffffffffu
#define HTCLIENT 1u
#define CS_DBLCLKS 0x0008u
#define DROPPED 0xffffffffu /* a message whose window is gone */

uint32_t rc_wnd_paint_next(rc_cpu *c, uint32_t hwnd);

/* ---- the queue ---- */

#define QUEUE 1024
typedef struct msg {
    uint32_t hwnd, message, wparam, lparam, time;
    int32_t x, y;
    uint32_t thread; /* whose queue it is in (the window's thread) */
} msg;
static msg queue[QUEUE];
static uint32_t qhead, qtail;
static int32_t mouse_x, mouse_y;
static uint8_t keys[256];
static uint32_t last_time, last_pos;

void rc_post_message(uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam)
{
    if (qtail - qhead == QUEUE) {
        return; /* full: Windows drops too */
    }
    uint32_t t = hwnd && rc_wnd_thread(hwnd) ? rc_wnd_thread(hwnd) : rc_thread_id();
    msg m = {hwnd, message, wparam, lparam, (uint32_t)rc_ticks_peek_ms(), mouse_x, mouse_y, t};
    /* Mouse moves coalesce, as Windows' do. */
    if (message == WM_MOUSEMOVE && qtail != qhead && queue[(qtail - 1) % QUEUE].message == WM_MOUSEMOVE &&
        queue[(qtail - 1) % QUEUE].hwnd == hwnd) {
        queue[(qtail - 1) % QUEUE] = m;
        return;
    }
    queue[qtail % QUEUE] = m;
    qtail++;
}

/* The next message to be read: what TranslateMessage generates. */
static void post_front(uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam)
{
    if (qtail - qhead == QUEUE) {
        return;
    }
    qhead--;
    queue[qhead % QUEUE] = (msg){hwnd, message, wparam, lparam, (uint32_t)rc_ticks_peek_ms(), mouse_x, mouse_y, rc_thread_id()};
}

static uint32_t mk_flags(void)
{
    uint32_t f = 0;
    if (keys[0x01] & 0x80) {
        f |= 0x01;
    }
    if (keys[0x02] & 0x80) {
        f |= 0x02;
    }
    if (keys[0x10] & 0x80) {
        f |= 0x04;
    }
    if (keys[0x11] & 0x80) {
        f |= 0x08;
    }
    if (keys[0x04] & 0x80) {
        f |= 0x10;
    }
    return f;
}

static uint32_t point(int32_t x, int32_t y) { return ((uint32_t)y << 16) | ((uint32_t)x & 0xffff); }

uint8_t rc_key_state(uint32_t vk) { return keys[vk & 0xff]; }

static int quit_requested;

/* Events injected by a script, delivered before the host's. */
#define INJECTED 64
static rc_event injected[INJECTED];
static uint32_t ihead, itail;

void rc_inject_event(const rc_event *e)
{
    if (itail - ihead < INJECTED) {
        injected[itail++ % INJECTED] = *e;
    }
}

static bool next_event(rc_event *e)
{
    if (ihead != itail) {
        *e = injected[ihead++ % INJECTED];
        return true;
    }
    if (!rc_plat_poll(e)) {
        return false;
    }
    rc_record_event(e);
    return true;
}

static bool (*input_filter)(const rc_event *e);
static int last_source;
static void (*pointer_map)(int *x, int *y);

void rc_set_pointer_map(void (*fn)(int *x, int *y)) { pointer_map = fn; }
void rc_set_input_filter(bool (*fn)(const rc_event *e)) { input_filter = fn; }
int rc_input_source(void) { return last_source; }

/* Who else sees the input as the program gets it (DirectInput). */
#define OBSERVERS 4
static void (*observers[OBSERVERS])(const rc_event *e);
static int nobservers;

void rc_add_input_observer(void (*fn)(const rc_event *e))
{
    if (nobservers < OBSERVERS) {
        observers[nobservers++] = fn;
    }
}

/* Key state and the key messages' lParam; the message to send (0 for none). */
static uint32_t key_event(const rc_event *e, uint32_t *lp)
{
    uint8_t *k = &keys[e->vk & 0xff];
    if (e->down && !(*k & 0x80)) {
        *k ^= 1; /* toggle state, as Caps Lock shows it */
    }
    int was_down = (*k & 0x80) != 0;
    *k = (uint8_t)((*k & 1) | (e->down ? 0x80 : 0));
    int alt = (keys[0x12] & 0x80) != 0;
    *lp = 1u | ((uint32_t)(e->scancode & 0xff) << 16) | ((uint32_t)e->extended << 24);
    if (alt) {
        *lp |= 1u << 29;
    }
    if (was_down) {
        *lp |= 1u << 30;
    }
    if (!e->down) {
        *lp |= 1u << 31;
    }
    return (alt || e->vk == 0x12 || e->vk == 0x79) ? (e->down ? WM_SYSKEYDOWN : WM_SYSKEYUP)
                                                   : (e->down ? WM_KEYDOWN : WM_KEYUP);
}

/* Input on our desktop waits here until the program reads its messages. */
#define RAW 256
static rc_event raw[RAW];
static uint32_t rhead, rtail;

void rc_input_deliver(const rc_event *ev)
{
    for (int i = 0; i < nobservers; i++) {
        observers[i](ev);
    }
    rc_event e = *ev;
    if (rc_desktop_active()) {
        if (rtail - rhead < RAW) {
            raw[rtail++ % RAW] = e;
        }
        return;
    }
    uint32_t main = rc_main_window();
    uint32_t to = rc_wnd_capture() ? rc_wnd_capture() : main;
    if (!to) {
        return;
    }
    if (e.type == RC_EV_MOUSE_MOVE || e.type == RC_EV_MOUSE_BUTTON || e.type == RC_EV_WHEEL) {
        /* On the screen, as Windows keeps the cursor: the window can be larger than the game's
         * frame (bars beside it), and a drag goes on outside the window. Tropico 2 crashes on a
         * point off its screen (no window is under it, and the window under the pointer is used
         * without a check). */
        uint32_t sw, sh;
        rc_ddraw_mode(&sw, &sh);
        if (sw && sh) {
            e.x = e.x < 0 ? 0 : e.x >= (int)sw ? (int)sw - 1 : e.x;
            e.y = e.y < 0 ? 0 : e.y >= (int)sh ? (int)sh - 1 : e.y;
        }
        if (pointer_map) {
            pointer_map(&e.x, &e.y);
        }
    }
    switch (e.type) {
    case RC_EV_QUIT:
        if (!quit_requested) {
            quit_requested = 1;
            rc_post_message(main, WM_CLOSE, 0, 0);
        }
        break;
    case RC_EV_MOUSE_MOVE:
        mouse_x = e.x;
        mouse_y = e.y;
        rc_post_message(to, WM_MOUSEMOVE, mk_flags(), point(e.x, e.y));
        break;
    case RC_EV_MOUSE_BUTTON: {
        static const uint8_t vk[4] = {0, 0x01, 0x02, 0x04};
        mouse_x = e.x;
        mouse_y = e.y;
        keys[vk[e.button & 3]] = e.down ? 0x80 : 0;
        /* WM_LBUTTONDOWN 0x201, RBUTTON 0x204, MBUTTON 0x207; up is +1. */
        uint32_t m = WM_LBUTTONDOWN + 3u * (uint32_t)(e.button - 1) + (e.down ? 0u : 1u);
        rc_post_message(to, m, mk_flags(), point(e.x, e.y));
        break;
    }
    case RC_EV_WHEEL:
        rc_post_message(to, WM_MOUSEWHEEL, ((uint32_t)(e.delta & 0xffff) << 16) | mk_flags(), point(e.x, e.y));
        break;
    case RC_EV_KEY: {
        uint32_t lp, m = key_event(&e, &lp);
        uint32_t f = rc_wnd_focus();
        rc_post_message(f ? f : main, m, (uint32_t)e.vk, lp);
        break;
    }
    case RC_EV_FOCUS:
        rc_post_message(main, WM_ACTIVATEAPP, (uint32_t)e.down, 1);
        break;
    default:
        break;
    }
}

void rc_pump_events(void)
{
    rc_event e;
    while (next_event(&e)) {
        if (e.type == RC_EV_MOUSE_MOVE || e.type == RC_EV_MOUSE_BUTTON || e.type == RC_EV_WHEEL) {
            last_source = (int)e.source;
        }
        if (input_filter && input_filter(&e)) {
            continue;
        }
        rc_input_deliver(&e);
    }
}

/* The window that gets the mouse at (x, y) on the desktop and its hit test: windows that answer
 * HTTRANSPARENT hand on to the window below (here: the parent). */
static uint32_t mouse_target(rc_cpu *c, int32_t x, int32_t y, uint32_t *hit)
{
    uint32_t cap = rc_wnd_capture();
    if (cap) {
        *hit = HTCLIENT;
        return cap;
    }
    uint32_t h = rc_wnd_from_point(x, y);
    while (h) {
        *hit = rc_wnd_send(c, h, WM_NCHITTEST, 0, point(x, y));
        if (*hit != HTTRANSPARENT) {
            return h;
        }
        h = rc_wnd_parent(h);
    }
    *hit = 0;
    return 0;
}

static uint32_t last_down_time, last_down_hwnd, last_down_msg;
static int32_t last_down_x, last_down_y;

/* Desktop input into messages (needs the guest for the questions asked on the way). */
static void route_raw(rc_cpu *c)
{
    while (rhead != rtail) {
        rc_event e = raw[rhead++ % RAW];
        switch (e.type) {
        case RC_EV_QUIT:
            if (!quit_requested) {
                quit_requested = 1;
                rc_post_message(rc_main_window(), WM_CLOSE, 0, 0);
            }
            break;
        case RC_EV_MOUSE_MOVE:
        case RC_EV_MOUSE_BUTTON: {
            mouse_x = e.x;
            mouse_y = e.y;
            if (e.type == RC_EV_MOUSE_BUTTON) {
                static const uint8_t vk[4] = {0, 0x01, 0x02, 0x04};
                keys[vk[e.button & 3]] = e.down ? 0x80 : 0;
            }
            uint32_t hit, h = mouse_target(c, e.x, e.y, &hit);
            if (!h) {
                break;
            }
            uint32_t base = e.type == RC_EV_MOUSE_MOVE ? WM_MOUSEMOVE
                                                       : WM_LBUTTONDOWN + 3u * (uint32_t)(e.button - 1) + (e.down ? 0u : 1u);
            uint32_t top = rc_wnd_top_level(h);
            if (!rc_wnd_capture() && !rc_wnd_enabled(top)) {
                break; /* a window disabled for a modal dialog */
            }
            rc_wnd_send(c, h, WM_SETCURSOR, h, hit | (base << 16));
            if (!rc_wnd_ok(h)) {
                break;
            }
            if (e.type == RC_EV_MOUSE_BUTTON && e.down && top != rc_wnd_active()) {
                uint32_t r = rc_wnd_send(c, h, WM_MOUSEACTIVATE, top, hit | (base << 16));
                if (r == 1 || r == 2) {
                    rc_wnd_activate(c, top);
                }
                if (r == 2 || r == 4 || !rc_wnd_ok(h)) {
                    break; /* ...ANDEAT */
                }
            }
            bool client = hit == HTCLIENT || rc_wnd_capture() == h;
            uint32_t m = base;
            if (e.type == RC_EV_MOUSE_BUTTON && e.down) {
                uint32_t now = (uint32_t)rc_ticks_peek_ms();
                rc_wclass *k = rc_class_find(rc_wnd_class_name(h));
                int32_t dx = e.x - last_down_x, dy = e.y - last_down_y;
                if (h == last_down_hwnd && base == last_down_msg && now - last_down_time < 500 && dx * dx + dy * dy <= 16 &&
                    (!client || (k && (k->style & CS_DBLCLKS)))) {
                    m = base + 2; /* WM_xBUTTONDBLCLK */
                    last_down_time = 0;
                } else {
                    last_down_time = now;
                }
                last_down_hwnd = h;
                last_down_msg = base;
                last_down_x = e.x;
                last_down_y = e.y;
            }
            if (client) {
                int32_t ox, oy;
                rc_wnd_client_origin(h, &ox, &oy);
                rc_post_message(h, m, mk_flags(), point(e.x - ox, e.y - oy));
            } else {
                rc_post_message(h, m - WM_MOUSEMOVE + WM_NCMOUSEMOVE, hit, point(e.x, e.y));
            }
            break;
        }
        case RC_EV_WHEEL: {
            uint32_t f = rc_wnd_focus() ? rc_wnd_focus() : rc_wnd_active();
            if (f) {
                rc_post_message(f, WM_MOUSEWHEEL, ((uint32_t)(e.delta & 0xffff) << 16) | mk_flags(), point(e.x, e.y));
            }
            break;
        }
        case RC_EV_KEY: {
            uint32_t lp, m = key_event(&e, &lp);
            uint32_t f = rc_wnd_focus() ? rc_wnd_focus() : rc_wnd_active();
            if (f) {
                rc_post_message(f, m, (uint32_t)e.vk, lp);
            }
            break;
        }
        default:
            break;
        }
    }
}

/* Work that needs a guest CPU and must happen regularly (sound callbacks): done whenever the
 * program looks for messages. */
#define HOOKS 8
static void (*pump_hooks[HOOKS])(rc_cpu *c);
static int npump_hooks;

void rc_add_pump_hook(void (*fn)(rc_cpu *c))
{
    if (npump_hooks < HOOKS) {
        pump_hooks[npump_hooks++] = fn;
    }
}

void rc_input_mouse(int *x, int *y)
{
    *x = mouse_x;
    *y = mouse_y;
}

/* ---- timers ---- */

#define TIMERS 128
static struct {
    uint32_t hwnd, id, elapse, proc, thread;
    uint64_t due;
    bool used;
} timers[TIMERS];
static uint32_t next_timer_id = 0x7f00;

/* SetTimer(hwnd, id, elapse, proc) */
static void k_SetTimer(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), id = RC_ARG(c, 1), elapse = RC_ARG(c, 2) ? RC_ARG(c, 2) : 1;
    int freei = -1;
    for (int i = 0; i < TIMERS; i++) {
        if (timers[i].used && timers[i].hwnd == hwnd && hwnd && timers[i].id == id) {
            freei = i;
            break;
        }
        if (!timers[i].used && freei < 0) {
            freei = i;
        }
    }
    if (freei < 0) {
        RC_RETURN(c, 0, 4);
    }
    if (!hwnd) {
        id = ++next_timer_id;
    }
    timers[freei].used = true;
    timers[freei].hwnd = hwnd;
    timers[freei].id = id;
    timers[freei].elapse = elapse;
    timers[freei].proc = RC_ARG(c, 3);
    timers[freei].thread = hwnd && rc_wnd_thread(hwnd) ? rc_wnd_thread(hwnd) : rc_thread_id();
    timers[freei].due = rc_ticks_peek_ms() + elapse;
    RC_RETURN(c, id ? id : 1, 4);
}

static void k_KillTimer(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), id = RC_ARG(c, 1);
    for (int i = 0; i < TIMERS; i++) {
        if (timers[i].used && timers[i].hwnd == hwnd && timers[i].id == id) {
            timers[i].used = false;
            RC_RETURN(c, 1, 2);
        }
    }
    RC_RETURN(c, 0, 2);
}

void rc_input_forget(uint32_t hwnd)
{
    for (int i = 0; i < TIMERS; i++) {
        if (timers[i].used && timers[i].hwnd == hwnd) {
            timers[i].used = false;
        }
    }
    for (uint32_t i = qhead; i != qtail; i++) {
        if (queue[i % QUEUE].hwnd == hwnd) {
            queue[i % QUEUE].message = DROPPED;
            queue[i % QUEUE].hwnd = 0;
        }
    }
    if (last_down_hwnd == hwnd) {
        last_down_hwnd = 0;
    }
}

/* ---- hooks ---- */

#define HOOK_SLOTS 32
/* Newest first; a hook belongs to a thread (0: all of them) and sees only that thread's events. */
static struct {
    int kind;
    uint32_t proc, handle, thread;
} hooks[HOOK_SLOTS];
static int nhooks_set;
static uint32_t next_hook = 0x00ab0000u;

/* SetWindowsHookExA(kind, proc, module, thread) */
static void k_SetWindowsHookExA(rc_cpu *c)
{
    if (nhooks_set == HOOK_SLOTS) {
        RC_RETURN(c, 0, 4);
    }
    memmove(&hooks[1], &hooks[0], sizeof hooks[0] * (size_t)nhooks_set);
    nhooks_set++;
    hooks[0].kind = (int)RC_ARG(c, 0);
    hooks[0].proc = RC_ARG(c, 1);
    hooks[0].handle = ++next_hook;
    hooks[0].thread = RC_ARG(c, 3);
    RC_RETURN(c, next_hook, 4);
}

static void k_UnhookWindowsHookEx(rc_cpu *c)
{
    for (int i = 0; i < nhooks_set; i++) {
        if (hooks[i].handle == RC_ARG(c, 0)) {
            memmove(&hooks[i], &hooks[i + 1], sizeof hooks[0] * (size_t)(nhooks_set - i - 1));
            nhooks_set--;
            RC_RETURN(c, 1, 1);
        }
    }
    RC_RETURN(c, 0, 1);
}

/* The hooks of `kind` for this thread from position `from` on: the result of the first (it
 * calls the next with CallNextHookEx). */
static uint32_t call_from(rc_cpu *c, int from, int kind, uint32_t code, uint32_t wp, uint32_t lp, bool *called)
{
    uint32_t me = rc_thread_id();
    for (int i = from; i < nhooks_set; i++) {
        if (hooks[i].kind == kind && (!hooks[i].thread || hooks[i].thread == me)) {
            uint32_t args[3] = {code, wp, lp};
            if (called) {
                *called = true;
            }
            return rc_call_guest(c, hooks[i].proc, 3, args);
        }
    }
    return 0;
}

uint32_t rc_call_hooks(rc_cpu *c, int kind, uint32_t code, uint32_t wp, uint32_t lp, bool *called)
{
    if (called) {
        *called = false;
    }
    return call_from(c, 0, kind, code, wp, lp, called);
}

/* CallNextHookEx(hook, code, wp, lp) */
static void k_CallNextHookEx(rc_cpu *c)
{
    for (int i = 0; i < nhooks_set; i++) {
        if (hooks[i].handle == RC_ARG(c, 0)) {
            RC_RETURN(c, call_from(c, i + 1, hooks[i].kind, RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3), NULL), 4);
        }
    }
    RC_RETURN(c, 0, 4);
}

/* ---- reading messages ---- */

static void fill_msg(uint32_t out, uint32_t hwnd, uint32_t message, uint32_t wp, uint32_t lp, uint32_t time,
                     int32_t x, int32_t y)
{
    rc_st32(out, hwnd);
    rc_st32(out + 4, message);
    rc_st32(out + 8, wp);
    rc_st32(out + 12, lp);
    rc_st32(out + 16, time);
    rc_st32(out + 20, (uint32_t)x);
    rc_st32(out + 24, (uint32_t)y);
    last_time = time;
    last_pos = point(x, y);
}

static bool for_window(uint32_t m_hwnd, uint32_t hwnd)
{
    if (!hwnd) {
        return true;
    }
    if (hwnd == 0xffffffffu) {
        return m_hwnd == 0;
    }
    for (uint32_t h = m_hwnd; h; h = rc_wnd_parent(h)) {
        if (h == hwnd) {
            return true;
        }
    }
    return false;
}

int rc_peek_message(rc_cpu *c, uint32_t out, uint32_t hwnd, uint32_t lo, uint32_t hi, bool remove)
{
    rc_pump_events();
    for (int h = 0; h < npump_hooks; h++) {
        pump_hooks[h](c);
    }
    rc_thread_yield();
    rc_wnd_process_sent(c);
    if (rc_desktop_active()) {
        route_raw(c);
    }
    bool any_range = !(lo || hi);
    uint32_t me = rc_thread_id();
    for (uint32_t i = qhead; i != qtail; i++) {
        msg m = queue[i % QUEUE];
        if (m.thread != me || !for_window(m.hwnd, hwnd) || (!any_range && (m.message < lo || m.message > hi))) {
            continue;
        }
        if (remove) {
            for (uint32_t j = i; j + 1 != qtail; j++) { /* messages stay in order */
                queue[j % QUEUE] = queue[(j + 1) % QUEUE];
            }
            qtail--;
        }
        if (m.message == DROPPED) {
            if (!remove) {
                queue[i % QUEUE].message = DROPPED;
            }
            continue; /* its window is gone */
        }
        fill_msg(out, m.hwnd, m.message, m.wparam, m.lparam, m.time, m.x, m.y);
        if (remove) {
            rc_call_hooks(c, 3 /* WH_GETMESSAGE */, 0, 1, out, NULL);
        }
        return 1;
    }
    /* An empty queue: WM_PAINT for a window that needs it (frames are painted on the way). */
    if (any_range || (WM_PAINT >= lo && WM_PAINT <= hi)) {
        uint32_t p = rc_wnd_paint_next(c, hwnd == 0xffffffffu ? 0 : hwnd);
        if (p) {
            fill_msg(out, p, WM_PAINT, 0, 0, (uint32_t)rc_ticks_peek_ms(), mouse_x, mouse_y);
            return 1;
        }
    }
    /* Then timers that are due. */
    if (any_range || (WM_TIMER >= lo && WM_TIMER <= hi)) {
        uint64_t now = rc_ticks_peek_ms();
        for (int i = 0; i < TIMERS; i++) {
            if (timers[i].used && now >= timers[i].due && timers[i].thread == me && for_window(timers[i].hwnd, hwnd)) {
                if (remove) {
                    timers[i].due = now + timers[i].elapse;
                }
                fill_msg(out, timers[i].hwnd, WM_TIMER, timers[i].id, timers[i].proc, (uint32_t)now, mouse_x, mouse_y);
                return 1;
            }
        }
    }
    rc_desktop_present(false);
    rc_thread_poll();
    return 0;
}

static void k_PeekMessageA(rc_cpu *c)
{
    int r = rc_peek_message(c, RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3), (RC_ARG(c, 4) & 1) != 0);
    RC_RETURN(c, (uint32_t)r, 5);
}

/* GetMessageA(msg, hwnd, min, max): waits for a message; 0 for WM_QUIT. */
static void k_GetMessageA(rc_cpu *c)
{
    uint32_t out = RC_ARG(c, 0);
    while (!rc_peek_message(c, out, RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3), true)) {
        rc_thread_sleep(1);
    }
    RC_RETURN(c, rc_ld32(out + 4) == WM_QUIT ? 0 : 1, 4);
}

/* WaitMessage: until there is one. */
static void k_WaitMessage(rc_cpu *c)
{
    uint32_t tmp = rc_heap_alloc(28, false);
    while (!rc_peek_message(c, tmp, 0, 0, 0, false)) {
        rc_thread_sleep(1);
    }
    rc_heap_free(tmp);
    RC_RETURN(c, 1, 0);
}

/* Whether a message waits in the queue (MsgWaitForMultipleObjects' input). */
static bool queue_has_input(void *ctx)
{
    rc_cpu *c = ctx;
    uint32_t tmp = rc_heap_alloc(28, false);
    bool any = rc_peek_message(c, tmp, 0, 0, 0, false) != 0;
    rc_heap_free(tmp);
    return any;
}

/* MsgWaitForMultipleObjects(n, handles, all, timeout, wake mask): a handle, or input
 * (WAIT_OBJECT_0 + n). */
static void k_MsgWaitForMultipleObjects(rc_cpu *c)
{
    uint32_t n = RC_ARG(c, 0), p = RC_ARG(c, 1);
    uint32_t h[64];
    n = n > 64 ? 64 : n;
    for (uint32_t i = 0; i < n; i++) {
        h[i] = rc_ld32(p + 4 * i);
    }
    RC_RETURN(c, rc_k32_wait(h, n, RC_ARG(c, 2) != 0, RC_ARG(c, 3), queue_has_input, c), 5);
}

static void k_GetMessageTime(rc_cpu *c) { RC_RETURN(c, last_time, 0); }
static void k_GetMessagePos(rc_cpu *c) { RC_RETURN(c, last_pos, 0); }

/* US layout: the characters keys give, without and with shift. */
static int key_char(uint32_t vk, int shift, int caps)
{
    if (vk >= 'A' && vk <= 'Z') {
        return (shift ^ caps) ? (int)vk : (int)vk + 32;
    }
    if (vk >= '0' && vk <= '9') {
        static const char sh[] = ")!@#$%^&*(";
        return shift ? sh[vk - '0'] : (int)vk;
    }
    if (vk >= 0x60 && vk <= 0x69) {
        return '0' + (int)(vk - 0x60);
    }
    switch (vk) {
    case 0x20: return ' ';
    case 0x0d: return '\r';
    case 0x08: return 8;
    case 0x09: return '\t';
    case 0x1b: return 27;
    case 0x6a: return '*';
    case 0x6b: return '+';
    case 0x6d: return '-';
    case 0x6e: return '.';
    case 0x6f: return '/';
    case 0xba: return shift ? ':' : ';';
    case 0xbb: return shift ? '+' : '=';
    case 0xbc: return shift ? '<' : ',';
    case 0xbd: return shift ? '_' : '-';
    case 0xbe: return shift ? '>' : '.';
    case 0xbf: return shift ? '?' : '/';
    case 0xc0: return shift ? '~' : '`';
    case 0xdb: return shift ? '{' : '[';
    case 0xdc: return shift ? '|' : '\\';
    case 0xdd: return shift ? '}' : ']';
    case 0xde: return shift ? '"' : '\'';
    default: return 0;
    }
}

/* TranslateMessage's work on a MSG. */
void rc_translate_key(uint32_t p)
{
    uint32_t m = rc_ld32(p + 4), vk = rc_ld32(p + 8);
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
        int ch = key_char(vk, (keys[0x10] & 0x80) != 0, keys[0x14] & 1);
        if (ch && !(keys[0x11] & 0x80)) {
            post_front(rc_ld32(p), m == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR, (uint32_t)ch, rc_ld32(p + 12));
        }
    }
}

static void k_TranslateMessage(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    uint32_t m = rc_ld32(p + 4), vk = rc_ld32(p + 8);
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
        int ch = key_char(vk, (keys[0x10] & 0x80) != 0, keys[0x14] & 1);
        if (ch && !(keys[0x11] & 0x80)) {
            post_front(rc_ld32(p), m == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR, (uint32_t)ch, rc_ld32(p + 12));
            RC_RETURN(c, 1, 1);
        }
    }
    RC_RETURN(c, 0, 1);
}

static void k_DispatchMessageA(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    uint32_t hwnd = rc_ld32(p), m = rc_ld32(p + 4), r = 0;
    if (m == WM_TIMER && rc_ld32(p + 12)) {
        uint32_t args[4] = {hwnd, WM_TIMER, rc_ld32(p + 8), (uint32_t)rc_ticks_peek_ms()};
        RC_RETURN(c, rc_call_guest(c, rc_ld32(p + 12), 4, args), 1);
    }
    if (rc_wnd_ok(hwnd)) {
        r = rc_wnd_send(c, hwnd, m, rc_ld32(p + 8), rc_ld32(p + 12));
        if (m == WM_PAINT && rc_wnd_ok(hwnd)) {
            rc_wnd_validate(hwnd); /* a program that does not validate gets no second WM_PAINT */
        }
    }
    if (rc_trace("messages") && m != WM_MOUSEMOVE) {
        rc_log("[msg] %#06x to %#x wp %#x lp %#x -> %#x", m, hwnd, rc_ld32(p + 8), rc_ld32(p + 12), r);
    }
    RC_RETURN(c, r, 1);
}

static void k_SendMessageA(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0);
    if (hwnd == 0xffffu) { /* HWND_BROADCAST: the top-level windows */
        for (uint32_t h = rc_wnd_first_child(0); h; h = rc_wnd_next_sibling(h)) {
            rc_wnd_send(c, h, RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3));
        }
        RC_RETURN(c, 0, 4);
    }
    RC_RETURN(c, rc_wnd_send(c, hwnd, RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3)), 4);
}

static void k_PostMessageA(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0);
    if (!hwnd && !rc_desktop_active()) {
        hwnd = rc_main_window(); /* as the games have always been answered */
    }
    rc_post_message(hwnd, RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3));
    RC_RETURN(c, 1, 4);
}

static void k_PostThreadMessageA(rc_cpu *c)
{
    if (qtail - qhead < QUEUE) {
        queue[qtail++ % QUEUE] = (msg){0, RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3), (uint32_t)rc_ticks_peek_ms(), mouse_x,
                                       mouse_y, RC_ARG(c, 0)};
    }
    RC_RETURN(c, 1, 4);
}


static void k_PostQuitMessage(rc_cpu *c)
{
    rc_post_message(0, WM_QUIT, RC_ARG(c, 0), 0);
    RC_RETURN(c, 0, 1);
}

static void k_InSendMessage(rc_cpu *c) { RC_RETURN(c, 0, 0); }

/* GetQueueStatus(flags): what is waiting (HIWORD) and new (LOWORD); roughly. */
static void k_GetQueueStatus(rc_cpu *c)
{
    rc_pump_events();
    uint32_t s = qhead != qtail || rhead != rtail ? 0x04ff : 0;
    RC_RETURN(c, (s << 16) | s, 1);
}

/* ---- modal loops ---- */

void rc_modal_loop(rc_cpu *c, uint32_t dialog, const bool *done)
{
    uint32_t m = rc_heap_alloc(28, false);
    while (!*done) {
        if (!rc_peek_message(c, m, 0, 0, 0, true)) {
            rc_thread_sleep(1);
            continue;
        }
        uint32_t message = rc_ld32(m + 4);
        if (message == WM_QUIT) {
            post_front(0, WM_QUIT, rc_ld32(m + 8), 0); /* for the program's own loop */
            break;
        }
        bool called = false;
        if (rc_call_hooks(c, -1 /* WH_MSGFILTER */, dialog ? 0 : 2, 0, m, &called) && called) {
            continue;
        }
        if (dialog && rc_is_dialog_message(c, dialog, m)) {
            continue;
        }
        uint32_t hwnd = rc_ld32(m), wp = rc_ld32(m + 8), lp = rc_ld32(m + 12);
        rc_translate_key(m);
        if (message == WM_TIMER && lp) {
            uint32_t targs[4] = {hwnd, WM_TIMER, wp, (uint32_t)rc_ticks_peek_ms()};
            rc_call_guest(c, lp, 4, targs);
        } else if (rc_wnd_ok(hwnd)) {
            rc_wnd_send(c, hwnd, message, wp, lp);
        }
    }
    rc_heap_free(m);
}

/* Follow the mouse while a button is held (dragging, menus, scroll bars): the next position;
 * false when the button is released. The desktop is presented meanwhile. */
bool rc_input_track(rc_cpu *c, int32_t *x, int32_t *y)
{
    for (;;) {
        rc_pump_events();
        while (rhead != rtail) {
            rc_event e = raw[rhead++ % RAW];
            if (e.type == RC_EV_MOUSE_MOVE) {
                mouse_x = *x = e.x;
                mouse_y = *y = e.y;
                return true;
            }
            if (e.type == RC_EV_MOUSE_BUTTON) {
                static const uint8_t vk[4] = {0, 0x01, 0x02, 0x04};
                keys[vk[e.button & 3]] = e.down ? 0x80 : 0;
                mouse_x = *x = e.x;
                mouse_y = *y = e.y;
                if (!e.down) {
                    return false;
                }
            }
            if (e.type == RC_EV_KEY && e.vk == 0x1b && e.down) {
                return false;
            }
            if (e.type == RC_EV_QUIT) {
                raw[--rhead % RAW] = e;
                return false;
            }
        }
        if (!(keys[0x01] & 0x80) && !(keys[0x02] & 0x80)) {
            return false;
        }
        rc_wnd_paint_all(c);
        rc_desktop_present(false);
        rc_thread_sleep(1);
    }
}

/* The next input event for a menu or other modal tracking (raw, on the desktop): false if none. */
bool rc_input_next_raw(rc_event *e)
{
    rc_pump_events();
    if (rhead == rtail) {
        return false;
    }
    *e = raw[rhead++ % RAW];
    if (e->type == RC_EV_MOUSE_MOVE || e->type == RC_EV_MOUSE_BUTTON) {
        mouse_x = e->x;
        mouse_y = e->y;
        if (e->type == RC_EV_MOUSE_BUTTON) {
            static const uint8_t vk[4] = {0, 0x01, 0x02, 0x04};
            keys[vk[e->button & 3]] = e->down ? 0x80 : 0;
        }
    } else if (e->type == RC_EV_KEY) {
        uint32_t lp;
        key_event(e, &lp);
    }
    return true;
}

/* An event a tracking loop does not want after all goes back. */
void rc_input_unread_raw(const rc_event *e)
{
    raw[--rhead % RAW] = *e;
}

/* ---- keys and the mouse ---- */

static void k_GetKeyboardState(rc_cpu *c)
{
    memcpy(rc_ptr(RC_ARG(c, 0)), keys, 256);
    RC_RETURN(c, 1, 1);
}

static void k_ToAscii(rc_cpu *c)
{
    uint32_t vk = RC_ARG(c, 0), state = RC_ARG(c, 2), out = RC_ARG(c, 3);
    int shift = (rc_ld8(state + 0x10) & 0x80) != 0, caps = rc_ld8(state + 0x14) & 1;
    int ch = (rc_ld8(state + 0x11) & 0x80) ? 0 : key_char(vk, shift, caps);
    if (!ch) {
        RC_RETURN(c, 0, 5);
    }
    rc_st16(out, (uint16_t)ch);
    RC_RETURN(c, 1, 5);
}

static void k_GetAsyncKeyState(rc_cpu *c)
{
    rc_pump_events();
    RC_RETURN(c, (keys[RC_ARG(c, 0) & 0xff] & 0x80) ? 0x8000u : 0, 1);
}

/* The key's state as of the last message: down (high bit) and toggled (low bit). */
static void k_GetKeyState(rc_cpu *c)
{
    uint8_t k = keys[RC_ARG(c, 0) & 0xff];
    RC_RETURN(c, ((k & 0x80) ? 0xff80u : 0) | (k & 1), 1);
}

static void k_GetCursorPos(rc_cpu *c)
{
    rc_st32(RC_ARG(c, 0), (uint32_t)mouse_x);
    rc_st32(RC_ARG(c, 0) + 4, (uint32_t)mouse_y);
    RC_RETURN(c, 1, 1);
}

/* The pointer goes where the program puts it, as far as the program sees it. */
static void k_SetCursorPos(rc_cpu *c)
{
    mouse_x = (int32_t)RC_ARG(c, 0);
    mouse_y = (int32_t)RC_ARG(c, 1);
    RC_RETURN(c, 1, 2);
}

/* The platform keeps the pointer in the window anyway. */
static void k_ClipCursor(rc_cpu *c) { RC_RETURN(c, 1, 1); }

/* ---- carets: kept, drawn by the controls that have them ---- */

static struct {
    uint32_t hwnd;
    int32_t x, y, w, h;
    int shown;
} caret;

static void k_CreateCaret(rc_cpu *c)
{
    caret.hwnd = RC_ARG(c, 0);
    caret.w = (int32_t)RC_ARG(c, 2);
    caret.h = (int32_t)RC_ARG(c, 3);
    caret.shown = 0;
    RC_RETURN(c, 1, 4);
}
static void k_DestroyCaret(rc_cpu *c)
{
    caret.hwnd = 0;
    RC_RETURN(c, 1, 0);
}
static void k_ShowCaret(rc_cpu *c)
{
    caret.shown++;
    RC_RETURN(c, 1, 1);
}
static void k_HideCaret(rc_cpu *c)
{
    caret.shown--;
    RC_RETURN(c, 1, 1);
}
static void k_SetCaretPos(rc_cpu *c)
{
    caret.x = (int32_t)RC_ARG(c, 0);
    caret.y = (int32_t)RC_ARG(c, 1);
    RC_RETURN(c, 1, 2);
}
static void k_GetCaretPos(rc_cpu *c)
{
    rc_st32(RC_ARG(c, 0), (uint32_t)caret.x);
    rc_st32(RC_ARG(c, 0) + 4, (uint32_t)caret.y);
    RC_RETURN(c, 1, 1);
}
static void k_GetCaretBlinkTime(rc_cpu *c) { RC_RETURN(c, 530, 0); }
static void k_GetDoubleClickTime(rc_cpu *c) { RC_RETURN(c, 500, 0); }

const rc_import_def rc_user32_input[] = {
    {U32, "PeekMessageA", k_PeekMessageA},
    {U32, "GetMessageA", k_GetMessageA},
    {U32, "WaitMessage", k_WaitMessage},
    {U32, "MsgWaitForMultipleObjects", k_MsgWaitForMultipleObjects},
    {U32, "GetMessageTime", k_GetMessageTime},
    {U32, "GetMessagePos", k_GetMessagePos},
    {U32, "TranslateMessage", k_TranslateMessage},
    {U32, "DispatchMessageA", k_DispatchMessageA},
    {U32, "SendMessageA", k_SendMessageA},
    {U32, "PostMessageA", k_PostMessageA},
    {U32, "PostThreadMessageA", k_PostThreadMessageA},
    {U32, "PostQuitMessage", k_PostQuitMessage},
    {U32, "InSendMessage", k_InSendMessage},
    {U32, "GetQueueStatus", k_GetQueueStatus},
    {U32, "SetTimer", k_SetTimer},
    {U32, "KillTimer", k_KillTimer},
    {U32, "SetWindowsHookExA", k_SetWindowsHookExA},
    {U32, "UnhookWindowsHookEx", k_UnhookWindowsHookEx},
    {U32, "CallNextHookEx", k_CallNextHookEx},
    {U32, "GetKeyboardState", k_GetKeyboardState},
    {U32, "ToAscii", k_ToAscii},
    {U32, "GetAsyncKeyState", k_GetAsyncKeyState},
    {U32, "GetKeyState", k_GetKeyState},
    {U32, "GetCursorPos", k_GetCursorPos},
    {U32, "SetCursorPos", k_SetCursorPos},
    {U32, "ClipCursor", k_ClipCursor},
    {U32, "CreateCaret", k_CreateCaret},
    {U32, "DestroyCaret", k_DestroyCaret},
    {U32, "ShowCaret", k_ShowCaret},
    {U32, "HideCaret", k_HideCaret},
    {U32, "SetCaretPos", k_SetCaretPos},
    {U32, "GetCaretPos", k_GetCaretPos},
    {U32, "GetCaretBlinkTime", k_GetCaretBlinkTime},
    {U32, "GetDoubleClickTime", k_GetDoubleClickTime},
};
const uint32_t rc_user32_input_count = sizeof rc_user32_input / sizeof rc_user32_input[0];
