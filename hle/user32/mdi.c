/* SPDX-License-Identifier: MIT */
/*
 * user32: the multiple-document interface (MFC's CMDIFrameWnd and CMDIChildWnd; Heroes IV's
 * campaign editor). The MDICLIENT window keeps the document windows: it creates them for
 * WM_MDICREATE, knows which is active, maximizes, arranges and destroys them; DefFrameProcA and
 * DefMDIChildProcA do the frame's and the children's part. Children are ordinary child windows
 * of the client with a caption; the window menu's list of documents is not kept.
 */
#include <stdio.h>
#include <string.h>

#include "rc/hle.h"
#include "rc/log.h"
#include "user32.h"

#define U32 "USER32.dll"

#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_SIZE 0x0005u
#define WM_SETFOCUS 0x0007u
#define WM_CLOSE 0x0010u
#define WM_NCACTIVATE 0x0086u
#define WM_CHILDACTIVATE 0x0022u
#define WM_MOUSEACTIVATE 0x0021u
#define WM_COMMAND 0x0111u
#define WM_MDICREATE 0x0220u
#define WM_MDIDESTROY 0x0221u
#define WM_MDIACTIVATE 0x0222u
#define WM_MDIRESTORE 0x0223u
#define WM_MDINEXT 0x0224u
#define WM_MDIMAXIMIZE 0x0225u
#define WM_MDITILE 0x0226u
#define WM_MDICASCADE 0x0227u
#define WM_MDIICONARRANGE 0x0228u
#define WM_MDIGETACTIVE 0x0229u
#define WM_MDISETMENU 0x0230u
#define WM_MDIREFRESHMENU 0x0234u

#define WS_CHILD 0x40000000u
#define WS_VISIBLE 0x10000000u
#define WS_CLIPSIBLINGS 0x04000000u
#define WS_OVERLAPPEDWINDOW 0x00cf0000u
#define WS_MAXIMIZE 0x01000000u
#define WS_EX_MDICHILD 0x40u
#define CW_USEDEFAULT 0x80000000u

#define CLIENTS 4
static struct mdi {
    uint32_t hwnd, active, first_id, window_menu, count;
    bool maximized;
} clients[CLIENTS];

static struct mdi *client_of(uint32_t hwnd, bool make)
{
    for (int i = 0; i < CLIENTS; i++) {
        if (clients[i].hwnd == hwnd && hwnd) {
            return &clients[i];
        }
    }
    for (int i = 0; make && i < CLIENTS; i++) {
        if (!clients[i].hwnd || !rc_wnd_ok(clients[i].hwnd)) {
            memset(&clients[i], 0, sizeof clients[i]);
            clients[i].hwnd = hwnd;
            return &clients[i];
        }
    }
    return NULL;
}

/* Make `child` the active document (0: none): WM_MDIACTIVATE to the old and the new one. */
static void activate(rc_cpu *c, struct mdi *m, uint32_t child)
{
    uint32_t old = m->active;
    if (old == child) {
        return;
    }
    m->active = child;
    if (old && rc_wnd_ok(old)) {
        rc_wnd_send(c, old, WM_NCACTIVATE, 0, 0);
        rc_wnd_send(c, old, WM_MDIACTIVATE, old, child);
    }
    if (child && rc_wnd_ok(child)) {
        rc_wnd_send(c, child, WM_NCACTIVATE, 1, 0);
        rc_wnd_send(c, child, WM_MDIACTIVATE, old, child);
        rc_wnd_set_focus(c, child);
    }
}

/* The document after `from` among the client's children (or the first). */
static uint32_t next_child(uint32_t client, uint32_t from)
{
    uint32_t first = 0;
    for (uint32_t h = rc_wnd_first_child(client); h; h = rc_wnd_next_sibling(h)) {
        if (h == from) {
            continue;
        }
        if (!first) {
            first = h;
        }
    }
    return first;
}

static void maximize(rc_cpu *c, struct mdi *m, uint32_t child)
{
    rc_rect r = rc_wnd_client(m->hwnd);
    rc_wnd_move(c, child, r.l, r.t, r.r - r.l, r.b - r.t, true);
    m->maximized = true;
}

static uint32_t create_child(rc_cpu *c, struct mdi *m, uint32_t mcs)
{
    const char *cls = rc_str(rc_ld32(mcs));
    char title[256];
    uint32_t t = rc_ld32(mcs + 4);
    snprintf(title, sizeof title, "%s", t ? rc_str(t) : "");
    int32_t x = (int32_t)rc_ld32(mcs + 12), y = (int32_t)rc_ld32(mcs + 16);
    int32_t w = (int32_t)rc_ld32(mcs + 20), h = (int32_t)rc_ld32(mcs + 24);
    uint32_t style = rc_ld32(mcs + 28);
    rc_rect area = rc_wnd_client(m->hwnd);
    int32_t step = 24 * (int32_t)(m->count % 8);
    if ((uint32_t)x == CW_USEDEFAULT) {
        x = step;
        y = step;
    }
    if ((uint32_t)w == CW_USEDEFAULT) {
        w = (area.r - area.l) * 3 / 4;
        h = (area.b - area.t) * 3 / 4;
    }
    uint32_t id = m->first_id + m->count++;
    uint32_t hwnd = rc_wnd_create(c, WS_EX_MDICHILD, cls, title,
                                  (style & ~WS_VISIBLE) | WS_CHILD | WS_CLIPSIBLINGS | WS_OVERLAPPEDWINDOW, x, y, w, h,
                                  m->hwnd, id, rc_ld32(mcs + 8), mcs);
    if (!hwnd) {
        return 0;
    }
    rc_wnd_show(c, hwnd, 5 /* SW_SHOW */);
    if ((style & WS_MAXIMIZE) || m->maximized) {
        maximize(c, m, hwnd);
    }
    activate(c, m, hwnd);
    return hwnd;
}

static void wp_mdiclient(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), msg = RC_ARG(c, 1), wp = RC_ARG(c, 2), lp = RC_ARG(c, 3);
    struct mdi *m = client_of(hwnd, msg == WM_CREATE);
    uint32_t r = 0;
    switch (msg) {
    case WM_CREATE:
        if (m && lp) {
            uint32_t ccs = rc_ld32(lp); /* CREATESTRUCT.lpCreateParams: CLIENTCREATESTRUCT */
            if (ccs) {
                m->window_menu = rc_ld32(ccs);
                m->first_id = rc_ld32(ccs + 4);
            }
        }
        break;
    case WM_DESTROY:
        if (m) {
            m->hwnd = 0;
        }
        r = rc_def_window_proc(c, hwnd, msg, wp, lp);
        break;
    case WM_MDICREATE:
        r = m && lp ? create_child(c, m, lp) : 0;
        break;
    case WM_MDIDESTROY:
        if (m && wp && rc_wnd_ok(wp)) {
            uint32_t next = m->active == wp ? next_child(hwnd, wp) : m->active;
            if (m->active == wp) {
                m->active = 0;
            }
            rc_wnd_destroy(c, wp);
            if (next) {
                activate(c, m, next);
            }
        }
        break;
    case WM_MDIACTIVATE:
        if (m && wp && rc_wnd_ok(wp)) {
            activate(c, m, wp);
        }
        break;
    case WM_MDINEXT:
        if (m) {
            uint32_t next = next_child(hwnd, wp ? wp : m->active);
            if (next) {
                activate(c, m, next);
            }
        }
        break;
    case WM_MDIMAXIMIZE:
        if (m && wp && rc_wnd_ok(wp)) {
            maximize(c, m, wp);
        }
        break;
    case WM_MDIRESTORE:
        if (m) {
            m->maximized = false;
        }
        break;
    case WM_MDICASCADE:
    case WM_MDITILE:
    case WM_MDIICONARRANGE:
        if (m) {
            rc_rect a = rc_wnd_client(hwnd);
            int32_t k = 0;
            m->maximized = false;
            for (uint32_t h = rc_wnd_first_child(hwnd); h; h = rc_wnd_next_sibling(h), k++) {
                rc_wnd_move(c, h, 24 * k, 24 * k, (a.r - a.l) * 3 / 4, (a.b - a.t) * 3 / 4, true);
            }
            r = 1;
        }
        break;
    case WM_MDIGETACTIVE:
        if (lp) {
            rc_st32(lp, m && m->maximized && m->active);
        }
        r = m ? m->active : 0;
        break;
    case WM_MDISETMENU:
    case WM_MDIREFRESHMENU: {
        uint32_t frame = rc_wnd_parent(hwnd);
        r = frame ? rc_wnd_menu(frame) : 0;
        if (msg == WM_MDISETMENU && frame && wp) {
            rc_wnd_set_menu(frame, wp);
            rc_wnd_frame_changed(c, frame);
        }
        if (msg == WM_MDISETMENU && m && lp) {
            m->window_menu = lp;
        }
        break;
    }
    case WM_SETFOCUS:
        if (m && m->active && rc_wnd_ok(m->active)) {
            rc_wnd_set_focus(c, m->active);
        }
        break;
    default:
        r = rc_def_window_proc(c, hwnd, msg, wp, lp);
        break;
    }
    RC_RETURN(c, r, 4);
}

/* DefFrameProcA(frame, client, msg, wp, lp) */
static void k_DefFrameProcA(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), client = RC_ARG(c, 1), msg = RC_ARG(c, 2), wp = RC_ARG(c, 3), lp = RC_ARG(c, 4);
    struct mdi *m = client ? client_of(client, false) : NULL;
    switch (msg) {
    case WM_SIZE:
        if (client && rc_wnd_ok(client)) {
            rc_wnd_move(c, client, 0, 0, (int32_t)(lp & 0xffff), (int32_t)(lp >> 16), true);
        }
        RC_RETURN(c, 0, 5);
    case WM_SETFOCUS:
        if (client && rc_wnd_ok(client)) {
            rc_wnd_set_focus(c, client);
        }
        RC_RETURN(c, 0, 5);
    case WM_COMMAND:
        /* a document chosen in the window menu */
        if (m && m->first_id && (wp & 0xffff) >= m->first_id && (wp & 0xffff) < m->first_id + m->count) {
            for (uint32_t h = rc_wnd_first_child(client); h; h = rc_wnd_next_sibling(h)) {
                if (rc_wnd_id(h) == (wp & 0xffff)) {
                    activate(c, m, h);
                }
            }
            RC_RETURN(c, 0, 5);
        }
        break;
    default:
        break;
    }
    RC_RETURN(c, rc_def_window_proc(c, hwnd, msg, wp, lp), 5);
}

/* DefMDIChildProcA(child, msg, wp, lp) */
static void k_DefMDIChildProcA(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), msg = RC_ARG(c, 1), wp = RC_ARG(c, 2), lp = RC_ARG(c, 3);
    uint32_t client = rc_wnd_parent(hwnd);
    struct mdi *m = client ? client_of(client, false) : NULL;
    switch (msg) {
    case WM_CHILDACTIVATE:
    case WM_SETFOCUS:
        if (m && m->active != hwnd) {
            activate(c, m, hwnd);
        }
        break;
    case WM_CLOSE:
        if (client) {
            rc_wnd_send(c, client, WM_MDIDESTROY, hwnd, 0);
        }
        RC_RETURN(c, 0, 4);
    default:
        break;
    }
    RC_RETURN(c, rc_def_window_proc(c, hwnd, msg, wp, lp), 4);
}

/* TranslateMDISysAccel(client, msg): the documents' system menu keys are not translated. */
static void k_TranslateMDISysAccel(rc_cpu *c) { RC_RETURN(c, 0, 2); }

static const rc_import_def defs[] = {
    {U32, "DefFrameProcA", k_DefFrameProcA},
    {U32, "DefMDIChildProcA", k_DefMDIChildProcA},
    {U32, "TranslateMDISysAccel", k_TranslateMDISysAccel},
};

void rc_mdi_init(void)
{
    rc_register_imports(defs, sizeof defs / sizeof defs[0]);
    rc_class_builtin("MDIClient", 0x8, wp_mdiclient, 0, 13 /* COLOR_APPWORKSPACE + 1 */, rc_arrow_cursor());
}
