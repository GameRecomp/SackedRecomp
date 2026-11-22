/* SPDX-License-Identifier: MIT */
/*
 * user32: menus. Menu objects with their items (from resource templates or built by calls), the
 * menu bar in a window's frame, and tracking: a modal loop over the desktop's raw input in which
 * popups are drawn straight onto the desktop (what was under them is put back afterwards). The
 * owner window gets WM_INITMENU, WM_INITMENUPOPUP (where programs enable and check items),
 * WM_MENUSELECT and finally WM_COMMAND.
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
#include "rc/thread.h"
#include "rc/vm.h"
#include "user32.h"

#define U32 "USER32.dll"

#define MF_GRAYED 0x1u
#define MF_DISABLED 0x2u
#define MF_BITMAP 0x4u
#define MF_CHECKED 0x8u
#define MF_POPUP 0x10u
#define MF_MENUBARBREAK 0x20u
#define MF_MENUBREAK 0x40u
#define MF_END 0x80u
#define MF_OWNERDRAW 0x100u
#define MF_BYPOSITION 0x400u
#define MF_SEPARATOR 0x800u
#define MF_DEFAULT 0x1000u
#define MF_HILITE 0x80u
#define MFT_RADIOCHECK 0x200u
#define MF_RIGHTJUSTIFY 0x4000u

#define WM_COMMAND 0x0111u
#define WM_SYSCOMMAND 0x0112u
#define WM_INITMENU 0x0116u
#define WM_INITMENUPOPUP 0x0117u
#define WM_MENUSELECT 0x011fu
#define WM_ENTERMENULOOP 0x0211u
#define WM_EXITMENULOOP 0x0212u
#define WM_ENTERIDLE 0x0121u

typedef struct item {
    uint32_t flags, id, sub, data;
    char *text;
} item;

typedef struct menu {
    bool used, popup;
    item *items;
    int n, cap;
    uint32_t default_id;
} menu;

#define MENUS 1024
#define MENU_BASE 0x00200000u
static menu menus[MENUS];

static menu *get(uint32_t h)
{
    if (h < MENU_BASE || (h - MENU_BASE) % 4) {
        return NULL;
    }
    uint32_t i = (h - MENU_BASE) / 4;
    return i < MENUS && menus[i].used ? &menus[i] : NULL;
}

bool rc_menu_valid(uint32_t h) { return get(h) != NULL; }

static uint32_t new_menu(bool popup)
{
    for (uint32_t i = 1; i < MENUS; i++) {
        if (!menus[i].used) {
            memset(&menus[i], 0, sizeof menus[i]);
            menus[i].used = true;
            menus[i].popup = popup;
            return MENU_BASE + 4 * i;
        }
    }
    rc_log("user32: out of menus");
    return 0;
}

static void free_menu(uint32_t h)
{
    menu *m = get(h);
    if (!m) {
        return;
    }
    for (int i = 0; i < m->n; i++) {
        if (m->items[i].flags & MF_POPUP) {
            free_menu(m->items[i].sub);
        }
        free(m->items[i].text);
    }
    free(m->items);
    m->used = false;
}

static item *insert_at(menu *m, int pos)
{
    if (m->n == m->cap) {
        m->cap = m->cap ? 2 * m->cap : 8;
        m->items = realloc(m->items, sizeof(item) * (size_t)m->cap);
    }
    if (pos < 0 || pos > m->n) {
        pos = m->n;
    }
    memmove(&m->items[pos + 1], &m->items[pos], sizeof(item) * (size_t)(m->n - pos));
    m->n++;
    memset(&m->items[pos], 0, sizeof(item));
    return &m->items[pos];
}

/* An item by command (searched in submenus too) or position; its menu in *owner. */
static item *find(uint32_t hmenu, uint32_t what, uint32_t flags, menu **owner, int *index)
{
    menu *m = get(hmenu);
    if (!m) {
        return NULL;
    }
    if (flags & MF_BYPOSITION) {
        if (what >= (uint32_t)m->n) {
            return NULL;
        }
        if (owner) {
            *owner = m;
        }
        if (index) {
            *index = (int)what;
        }
        return &m->items[what];
    }
    for (int i = 0; i < m->n; i++) {
        item *it = &m->items[i];
        if (!(it->flags & MF_POPUP) && it->id == what) {
            if (owner) {
                *owner = m;
            }
            if (index) {
                *index = i;
            }
            return it;
        }
        if (it->flags & MF_POPUP) {
            item *k = find(it->sub, what, flags, owner, index);
            if (k) {
                return k;
            }
            if (it->sub == what) { /* a popup's handle as its command */
                if (owner) {
                    *owner = m;
                }
                if (index) {
                    *index = i;
                }
                return it;
            }
        }
    }
    return NULL;
}

static void set_item(item *it, uint32_t flags, uint32_t id, uint32_t text)
{
    free(it->text);
    it->text = NULL;
    it->flags = flags & ~MF_BYPOSITION;
    if (flags & MF_POPUP) {
        it->sub = id;
        it->id = id;
    } else {
        it->id = id;
    }
    if (!(flags & (MF_SEPARATOR | MF_BITMAP | MF_OWNERDRAW)) && text) {
        it->text = strdup(text < 0x10000 ? "" : rc_str(text));
    } else {
        it->data = text;
    }
}

/* ---- resources ---- */

/* A menu template's items (MENUITEMTEMPLATE: flags, id unless a popup, UTF-16 text) at `p`. */
static uint32_t parse(uint32_t p, uint32_t hmenu)
{
    menu *m = get(hmenu);
    for (;;) {
        uint32_t flags = rc_ld16(p);
        p += 2;
        uint32_t id = 0;
        if (!(flags & MF_POPUP)) {
            id = rc_ld16(p);
            p += 2;
        }
        char text[256];
        int k = 0;
        for (uint32_t ch; (ch = rc_ld16(p)) != 0; p += 2) {
            if (k < 255) {
                text[k++] = (char)(ch < 256 ? ch : '?');
            }
        }
        text[k] = 0;
        p += 2;
        item *it = insert_at(m, -1);
        it->flags = flags & ~MF_END;
        if (!k && !(flags & MF_POPUP) && !id) {
            it->flags |= MF_SEPARATOR;
        } else {
            it->text = strdup(text);
        }
        if (flags & MF_POPUP) {
            uint32_t sub = new_menu(true);
            p = parse(p, sub);
            m = get(hmenu);
            it = &m->items[m->n - 1];
            it->sub = sub;
            it->id = sub;
        } else {
            it->id = id;
        }
        if (flags & MF_END) {
            return p;
        }
    }
}

static uint32_t load_template(uint32_t p)
{
    uint32_t version = rc_ld16(p), offset = rc_ld16(p + 2);
    uint32_t h = new_menu(false);
    if (version == 0) {
        parse(p + 4 + offset, h);
    } else {
        rc_log("LoadMenu: extended menu templates are not read");
    }
    return h;
}

uint32_t rc_menu_load(uint32_t inst, uint32_t name)
{
    uint32_t data, size;
    bool found = name < 0x10000 ? rc_resource(inst, 4, name, &data, &size)
                                : rc_resource_named(inst, 4, rc_str(name), &data, &size);
    return found ? load_template(data) : 0;
}

static void k_LoadMenuA(rc_cpu *c) { RC_RETURN(c, rc_menu_load(RC_ARG(c, 0), RC_ARG(c, 1)), 2); }

static void k_LoadMenuIndirectA(rc_cpu *c) { RC_RETURN(c, load_template(RC_ARG(c, 0)), 1); }
static void k_CreateMenu(rc_cpu *c) { RC_RETURN(c, new_menu(false), 0); }
static void k_CreatePopupMenu(rc_cpu *c) { RC_RETURN(c, new_menu(true), 0); }
static void k_DestroyMenu(rc_cpu *c)
{
    bool ok = get(RC_ARG(c, 0)) != NULL;
    free_menu(RC_ARG(c, 0));
    RC_RETURN(c, ok, 1);
}
static void k_IsMenu(rc_cpu *c) { RC_RETURN(c, get(RC_ARG(c, 0)) != NULL, 1); }

/* AppendMenuA(menu, flags, id, text) */
static void k_AppendMenuA(rc_cpu *c)
{
    menu *m = get(RC_ARG(c, 0));
    if (!m) {
        RC_RETURN(c, 0, 4);
    }
    set_item(insert_at(m, -1), RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3));
    RC_RETURN(c, 1, 4);
}

/* InsertMenuA(menu, position, flags, id, text) */
static void k_InsertMenuA(rc_cpu *c)
{
    menu *m = get(RC_ARG(c, 0));
    uint32_t pos = RC_ARG(c, 1), flags = RC_ARG(c, 2);
    if (!m) {
        RC_RETURN(c, 0, 5);
    }
    int at = -1;
    menu *owner = m;
    if (pos != 0xffffffffu) {
        if (flags & MF_BYPOSITION) {
            at = (int)pos;
        } else if (!find(RC_ARG(c, 0), pos, 0, &owner, &at)) {
            owner = m;
            at = -1;
        }
    }
    set_item(insert_at(owner, at), flags, RC_ARG(c, 3), RC_ARG(c, 4));
    RC_RETURN(c, 1, 5);
}

/* ModifyMenuA(menu, position, flags, id, text) */
static void k_ModifyMenuA(rc_cpu *c)
{
    item *it = find(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2), NULL, NULL);
    if (!it) {
        RC_RETURN(c, 0, 5);
    }
    set_item(it, RC_ARG(c, 2), RC_ARG(c, 3), RC_ARG(c, 4));
    RC_RETURN(c, 1, 5);
}

static uint32_t remove_item(uint32_t hmenu, uint32_t what, uint32_t flags, bool destroy)
{
    menu *owner;
    int at;
    item *it = find(hmenu, what, flags, &owner, &at);
    if (!it) {
        return 0;
    }
    if (destroy && (it->flags & MF_POPUP)) {
        free_menu(it->sub);
    }
    free(it->text);
    memmove(&owner->items[at], &owner->items[at + 1], sizeof(item) * (size_t)(owner->n - at - 1));
    owner->n--;
    return 1;
}

static void k_DeleteMenu(rc_cpu *c) { RC_RETURN(c, remove_item(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2), true), 3); }
static void k_RemoveMenu(rc_cpu *c) { RC_RETURN(c, remove_item(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2), false), 3); }

static void k_GetSubMenu(rc_cpu *c)
{
    menu *m = get(RC_ARG(c, 0));
    uint32_t i = RC_ARG(c, 1);
    RC_RETURN(c, m && i < (uint32_t)m->n && (m->items[i].flags & MF_POPUP) ? m->items[i].sub : 0, 2);
}

static void k_GetMenuItemCount(rc_cpu *c)
{
    menu *m = get(RC_ARG(c, 0));
    RC_RETURN(c, m ? (uint32_t)m->n : 0xffffffffu, 1);
}

static void k_GetMenuItemID(rc_cpu *c)
{
    menu *m = get(RC_ARG(c, 0));
    uint32_t i = RC_ARG(c, 1);
    if (!m || i >= (uint32_t)m->n) {
        RC_RETURN(c, 0xffffffffu, 2);
    }
    RC_RETURN(c, (m->items[i].flags & MF_POPUP) ? 0xffffffffu : m->items[i].id, 2);
}

/* GetMenuState(menu, item, flags): the flags (a popup: its item count in the high byte). */
static void k_GetMenuState(rc_cpu *c)
{
    item *it = find(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2), NULL, NULL);
    if (!it) {
        RC_RETURN(c, 0xffffffffu, 3);
    }
    uint32_t f = it->flags & ~MF_END;
    if (it->flags & MF_POPUP) {
        menu *s = get(it->sub);
        f |= (uint32_t)(s ? s->n : 0) << 8;
    }
    RC_RETURN(c, f, 3);
}

/* GetMenuStringA(menu, item, buffer, size, flags) */
static void k_GetMenuStringA(rc_cpu *c)
{
    item *it = find(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 4), NULL, NULL);
    const char *t = it && it->text ? it->text : "";
    if (!RC_ARG(c, 2)) {
        RC_RETURN(c, it ? (uint32_t)strlen(t) : 0, 5);
    }
    RC_RETURN(c, it ? rc_put_str(RC_ARG(c, 2), RC_ARG(c, 3), t) : 0, 5);
}

static void k_CheckMenuItem(rc_cpu *c)
{
    item *it = find(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2), NULL, NULL);
    if (!it) {
        RC_RETURN(c, 0xffffffffu, 3);
    }
    uint32_t old = it->flags & MF_CHECKED;
    it->flags = (RC_ARG(c, 2) & MF_CHECKED) ? it->flags | MF_CHECKED : it->flags & ~MF_CHECKED;
    RC_RETURN(c, old, 3);
}

static void k_EnableMenuItem(rc_cpu *c)
{
    item *it = find(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2), NULL, NULL);
    if (!it) {
        RC_RETURN(c, 0xffffffffu, 3);
    }
    uint32_t old = it->flags & (MF_GRAYED | MF_DISABLED);
    it->flags = (it->flags & ~(MF_GRAYED | MF_DISABLED)) | (RC_ARG(c, 2) & (MF_GRAYED | MF_DISABLED));
    RC_RETURN(c, old, 3);
}

/* CheckMenuRadioItem(menu, first, last, check, flags) */
static void k_CheckMenuRadioItem(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0), first = RC_ARG(c, 1), last = RC_ARG(c, 2), check = RC_ARG(c, 3), flags = RC_ARG(c, 4);
    for (uint32_t i = first; i <= last; i++) {
        item *it = find(h, i, flags, NULL, NULL);
        if (it) {
            it->flags = i == check ? it->flags | MF_CHECKED | MFT_RADIOCHECK : it->flags & ~MF_CHECKED;
        }
    }
    RC_RETURN(c, 1, 5);
}

static void k_GetMenuCheckMarkDimensions(rc_cpu *c) { RC_RETURN(c, (13u << 16) | 13u, 0); }
static void k_SetMenuItemBitmaps(rc_cpu *c) { RC_RETURN(c, 1, 5); }
static void k_HiliteMenuItem(rc_cpu *c) { RC_RETURN(c, 1, 4); }
static void k_SetMenuDefaultItem(rc_cpu *c)
{
    menu *m = get(RC_ARG(c, 0));
    if (m) {
        m->default_id = RC_ARG(c, 1);
    }
    RC_RETURN(c, m != NULL, 3);
}
static void k_GetMenuDefaultItem(rc_cpu *c)
{
    menu *m = get(RC_ARG(c, 0));
    RC_RETURN(c, m && m->default_id ? m->default_id : 0xffffffffu, 3);
}

/* The system menu: Close only. */
static void k_GetSystemMenu(rc_cpu *c)
{
    static uint32_t sys;
    if (!sys || !get(sys)) {
        sys = new_menu(true);
        item *it = insert_at(get(sys), -1);
        it->id = 0xf060; /* SC_CLOSE */
        it->text = strdup("&Close\tAlt+F4");
    }
    RC_RETURN(c, sys, 2);
}

/* MENUITEMINFOA: size, mask, type, state, id, submenu, checked, unchecked, data, text, cch. */
static void k_GetMenuItemInfoA(rc_cpu *c)
{
    item *it = find(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2) ? MF_BYPOSITION : 0, NULL, NULL);
    uint32_t p = RC_ARG(c, 3);
    if (!it) {
        RC_RETURN(c, 0, 4);
    }
    uint32_t mask = rc_ld32(p + 4);
    rc_st32(p + 8, it->flags & (MF_SEPARATOR | MF_BITMAP | MF_OWNERDRAW | MFT_RADIOCHECK | MF_MENUBREAK | MF_MENUBARBREAK));
    rc_st32(p + 12, it->flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_DEFAULT));
    rc_st32(p + 16, it->id);
    rc_st32(p + 20, (it->flags & MF_POPUP) ? it->sub : 0);
    rc_st32(p + 32, it->data);
    if ((mask & 0x10 /* MIIM_TYPE */) || (mask & 0x40 /* MIIM_STRING */)) {
        uint32_t buf = rc_ld32(p + 36), cch = rc_ld32(p + 40);
        const char *t = it->text ? it->text : "";
        if (buf && cch) {
            rc_put_str(buf, cch, t);
        }
        rc_st32(p + 40, (uint32_t)strlen(t));
    }
    RC_RETURN(c, 1, 4);
}

static void k_SetMenuItemInfoA(rc_cpu *c)
{
    item *it = find(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2) ? MF_BYPOSITION : 0, NULL, NULL);
    uint32_t p = RC_ARG(c, 3);
    if (!it) {
        RC_RETURN(c, 0, 4);
    }
    uint32_t mask = rc_ld32(p + 4);
    if (mask & 0x1) { /* MIIM_STATE */
        it->flags = (it->flags & ~(MF_GRAYED | MF_DISABLED | MF_CHECKED)) | (rc_ld32(p + 12) & (MF_GRAYED | MF_DISABLED | MF_CHECKED));
    }
    if (mask & 0x2) { /* MIIM_ID */
        it->id = rc_ld32(p + 16);
    }
    if (mask & 0x4) { /* MIIM_SUBMENU */
        it->sub = rc_ld32(p + 20);
        it->flags = it->sub ? it->flags | MF_POPUP : it->flags & ~MF_POPUP;
    }
    if (mask & 0x20) { /* MIIM_DATA */
        it->data = rc_ld32(p + 32);
    }
    if ((mask & 0x50) && rc_ld32(p + 36) >= 0x10000 && !(rc_ld32(p + 8) & MF_SEPARATOR)) {
        free(it->text);
        it->text = strdup(rc_str(rc_ld32(p + 36)));
    }
    RC_RETURN(c, 1, 4);
}

/* ---- drawing ---- */

#define BAR_H 19
#define ITEM_H 18
#define SEP_H 8

static uint32_t menu_font(void) { return rc_gui_font(); }

static void text_parts(const char *t, char *label, char *accel)
{
    const char *tab = strchr(t, '\t');
    size_t n = tab ? (size_t)(tab - t) : strlen(t);
    snprintf(label, 256, "%.*s", (int)n, t);
    snprintf(accel, 64, "%s", tab ? tab + 1 : "");
}

static int text_w(rc_dc *d, const char *t)
{
    rc_rect r = {0, 0, 0x3fffffff, 0x3fffffff};
    rc_draw_text(d, t, -1, &r, 0x420 /* CALCRECT SINGLELINE */);
    return r.r;
}

int rc_menu_bar_height(uint32_t hwnd)
{
    return get(rc_wnd_menu(hwnd)) ? BAR_H + 1 : 0;
}

/* The bar's items' rectangles (relative to the bar). */
static int bar_layout(rc_dc *d, menu *m, rc_rect *out, int cap)
{
    int32_t x = 0;
    int n = 0;
    for (int i = 0; i < m->n && n < cap; i++, n++) {
        int w = text_w(d, m->items[i].text ? m->items[i].text : "") + 14;
        out[n] = (rc_rect){x, 0, x + w, BAR_H};
        x += w;
    }
    return n;
}

static int bar_hot = -1;
static uint32_t bar_hwnd;

void rc_menu_draw_bar(rc_dc *d, uint32_t hwnd, rc_rect r)
{
    menu *m = get(rc_wnd_menu(hwnd));
    rc_dc_fill(d, r, rc_sys_color(4));
    rc_dc_fill(d, (rc_rect){r.l, r.b - 1, r.r, r.b}, rc_sys_color(15));
    if (!m) {
        return;
    }
    uint32_t old = d->font;
    d->font = menu_font();
    d->bk_mode = 1;
    rc_rect items[64];
    int n = bar_layout(d, m, items, 64);
    for (int i = 0; i < n; i++) {
        rc_rect ir = {r.l + items[i].l, r.t + items[i].t, r.l + items[i].r, r.t + items[i].b};
        bool hot = hwnd == bar_hwnd && i == bar_hot;
        if (hot) {
            rc_draw_edge(d, ir, 0x2 /* BDR_SUNKENOUTER */, 0xf);
        }
        bool gray = m->items[i].flags & (MF_GRAYED | MF_DISABLED);
        d->text_color = rc_sys_color(gray ? 17 : 7);
        rc_rect tr = ir;
        if (hot) {
            tr.l++, tr.t++;
        }
        rc_draw_text(d, m->items[i].text ? m->items[i].text : "", -1, &tr, 0x25 /* CENTER VCENTER SINGLELINE */);
    }
    d->font = old;
}

/* ---- popups (drawn on the desktop while a menu is tracked) ---- */

typedef struct popup {
    uint32_t hmenu;
    rc_rect r;  /* screen */
    int sel;
    uint32_t *saved; /* the pixels under it */
} popup;

static int item_h(const item *it) { return (it->flags & MF_SEPARATOR) ? SEP_H : ITEM_H; }

static rc_rect popup_size(rc_dc *d, menu *m)
{
    int w = 80, lw = 0, aw = 0, h = 4;
    for (int i = 0; i < m->n; i++) {
        char label[256], accel[64];
        text_parts(m->items[i].text ? m->items[i].text : "", label, accel);
        int a = text_w(d, label), b = accel[0] ? text_w(d, accel) : 0;
        lw = a > lw ? a : lw;
        aw = b > aw ? b : aw;
        h += item_h(&m->items[i]);
    }
    w = lw + aw + 20 + 20 + (aw ? 16 : 0);
    return (rc_rect){0, 0, w < 80 ? 80 : w, h};
}

static rc_rect item_rect(const popup *p, int i)
{
    menu *m = get(p->hmenu);
    int32_t y = p->r.t + 2;
    for (int k = 0; m && k < i; k++) {
        y += item_h(&m->items[k]);
    }
    return (rc_rect){p->r.l + 2, y, p->r.r - 2, y + (m && i < m->n ? item_h(&m->items[i]) : 0)};
}

static void draw_popup(rc_dc *d, popup *p)
{
    menu *m = get(p->hmenu);
    if (!m) {
        return;
    }
    rc_dc_fill(d, p->r, rc_sys_color(4));
    rc_draw_edge(d, p->r, 0x5, 0xf);
    uint32_t old = d->font;
    d->font = menu_font();
    d->bk_mode = 1;
    for (int i = 0; i < m->n; i++) {
        item *it = &m->items[i];
        rc_rect r = item_rect(p, i);
        if (it->flags & MF_SEPARATOR) {
            rc_draw_edge(d, (rc_rect){r.l + 1, r.t + 3, r.r - 1, r.t + 5}, 0x2 | 0x4, 0x2 | 0x8);
            continue;
        }
        bool sel = i == p->sel, gray = it->flags & (MF_GRAYED | MF_DISABLED);
        if (sel) {
            rc_dc_fill(d, r, rc_sys_color(13));
        }
        uint32_t color = gray ? rc_sys_color(17) : rc_sys_color(sel ? 14 : 7);
        d->text_color = color;
        if (it->flags & MF_CHECKED) {
            int32_t cx = r.l + 8, cy = r.t + 9;
            if (it->flags & MFT_RADIOCHECK) {
                rc_dc_fill(d, (rc_rect){cx - 2, cy - 2, cx + 3, cy + 3}, color);
            } else {
                for (int k = 0; k < 3; k++) {
                    rc_dc_fill(d, (rc_rect){cx - 3 + k, cy - 1 + k, cx - 2 + k, cy + 2 + k}, color);
                }
                for (int k = 0; k < 5; k++) {
                    rc_dc_fill(d, (rc_rect){cx + k, cy + 1 - k, cx + k + 1, cy + 4 - k}, color);
                }
            }
        }
        char label[256], accel[64];
        text_parts(it->text ? it->text : "", label, accel);
        rc_rect tr = {r.l + 18, r.t, r.r - 12, r.b};
        rc_draw_text(d, label, -1, &tr, 0x24);
        if (accel[0]) {
            rc_draw_text(d, accel, -1, &tr, 0x26 | 0x800);
        }
        if (it->flags & MF_POPUP) {
            int32_t ax = r.r - 8, ay = r.t + 5;
            for (int k = 0; k < 4; k++) {
                rc_dc_fill(d, (rc_rect){ax + k - 2, ay + k, ax + k - 1, ay + 8 - k}, color);
            }
        }
    }
    d->font = old;
}

static void save_under(rc_dc *d, popup *p)
{
    int32_t w = p->r.r - p->r.l, h = p->r.b - p->r.t;
    p->saved = malloc(sizeof(uint32_t) * (size_t)(w * h));
    for (int32_t y = 0; p->saved && y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            p->saved[y * w + x] = rc_dc_read(d, p->r.l + x, p->r.t + y);
        }
    }
}

static void restore_under(rc_dc *d, popup *p)
{
    int32_t w = p->r.r - p->r.l, h = p->r.b - p->r.t;
    for (int32_t y = 0; p->saved && y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            rc_dc_write(d, p->r.l + x, p->r.t + y, p->saved[y * w + x]);
        }
    }
    free(p->saved);
    p->saved = NULL;
}

/* ---- tracking ---- */

typedef struct tracker {
    rc_cpu *c;
    uint32_t owner;  /* gets the messages */
    uint32_t bar;    /* the menu bar's window, or 0 for a popup menu */
    rc_dc *d;
    popup stack[8];
    int depth;
    uint32_t chosen;
    bool done;
} tracker;

static void select_item(tracker *t, int level, int sel)
{
    popup *p = &t->stack[level];
    if (p->sel == sel) {
        return;
    }
    p->sel = sel;
    menu *m = get(p->hmenu);
    if (m && sel >= 0 && sel < m->n) {
        item *it = &m->items[sel];
        uint32_t flags = (it->flags & 0xffff) | 0x80 /* MF_HILITE */;
        rc_wnd_send(t->c, t->owner, WM_MENUSELECT, ((it->flags & MF_POPUP) ? (uint32_t)sel : it->id & 0xffff) | (flags << 16),
                    p->hmenu);
    }
    draw_popup(t->d, p);
}

static void close_to(tracker *t, int depth)
{
    while (t->depth > depth) {
        t->depth--;
        restore_under(t->d, &t->stack[t->depth]);
    }
}

static void open_popup(tracker *t, uint32_t hmenu, int32_t x, int32_t y, int index)
{
    if (t->depth == 8 || !get(hmenu)) {
        return;
    }
    rc_wnd_send(t->c, t->owner, WM_INITMENUPOPUP, hmenu, (uint32_t)index & 0xffff);
    menu *m = get(hmenu);
    if (!m || !m->n) {
        return;
    }
    rc_rect sz = popup_size(t->d, m);
    int32_t dw, dh;
    rc_desktop_size(&dw, &dh);
    if (x + sz.r > dw) {
        x = dw - sz.r;
    }
    if (y + sz.b > dh) {
        y = dh - sz.b;
    }
    popup *p = &t->stack[t->depth++];
    p->hmenu = hmenu;
    p->r = (rc_rect){x < 0 ? 0 : x, y < 0 ? 0 : y, (x < 0 ? 0 : x) + sz.r, (y < 0 ? 0 : y) + sz.b};
    p->sel = -1;
    save_under(t->d, p);
    draw_popup(t->d, p);
}

/* The bar's item rectangles on the screen. */
static int bar_items(tracker *t, rc_rect *out)
{
    menu *m = get(rc_wnd_menu(t->bar));
    if (!m) {
        return 0;
    }
    rc_rect wr = rc_wnd_screen_rect(t->bar);
    int32_t cx, cy;
    rc_wnd_client_origin(t->bar, &cx, &cy);
    int32_t top = cy - rc_menu_bar_height(t->bar);
    int32_t left = wr.l + (cx - wr.l > 4 ? 4 : cx - wr.l);
    uint32_t old = t->d->font;
    t->d->font = menu_font();
    int n = bar_layout(t->d, m, out, 64);
    t->d->font = old;
    for (int i = 0; i < n; i++) {
        out[i].l += left;
        out[i].r += left;
        out[i].t += top;
        out[i].b += top;
    }
    return n;
}

static void redraw_bar(tracker *t)
{
    if (t->bar) {
        uint32_t hdc = rc_wnd_dc(t->bar, true);
        rc_dc *d = rc_dc_get(hdc);
        if (d) {
            rc_rect wr = rc_wnd_screen_rect(t->bar);
            int32_t cx, cy;
            rc_wnd_client_origin(t->bar, &cx, &cy);
            int32_t bh = rc_menu_bar_height(t->bar), b = cx - wr.l;
            rc_menu_draw_bar(d, t->bar, (rc_rect){b, cy - bh - wr.t, wr.r - wr.l - b, cy - wr.t});
        }
        rc_wnd_release_dc(hdc);
    }
}

static void open_bar_item(tracker *t, int i)
{
    rc_rect items[64];
    int n = bar_items(t, items);
    if (i < 0 || i >= n) {
        return;
    }
    close_to(t, 0);
    bar_hot = i;
    bar_hwnd = t->bar;
    redraw_bar(t);
    menu *m = get(rc_wnd_menu(t->bar));
    item *it = &m->items[i];
    rc_wnd_send(t->c, t->owner, WM_MENUSELECT, (uint32_t)i | (((it->flags & 0xffff) | 0x80) << 16), rc_wnd_menu(t->bar));
    if (it->flags & MF_POPUP) {
        open_popup(t, it->sub, items[i].l, items[i].b, i);
    }
}

static void choose(tracker *t, item *it)
{
    if (it->flags & (MF_GRAYED | MF_DISABLED | MF_SEPARATOR | MF_POPUP)) {
        return;
    }
    t->chosen = it->id;
    t->done = true;
}

static bool at_item(tracker *t, int32_t x, int32_t y, int *level, int *index)
{
    for (int l = t->depth - 1; l >= 0; l--) {
        popup *p = &t->stack[l];
        menu *m = get(p->hmenu);
        if (x < p->r.l || x >= p->r.r || y < p->r.t || y >= p->r.b || !m) {
            continue;
        }
        for (int i = 0; i < m->n; i++) {
            rc_rect r = item_rect(p, i);
            if (y >= r.t && y < r.b) {
                *level = l;
                *index = i;
                return true;
            }
        }
        *level = l;
        *index = -1;
        return true;
    }
    return false;
}

/* The menu loop: `bar_index` the bar item to open first (or -1), `popup_menu`/x/y a popup to
 * show. Returns the chosen command (0: none). */
static uint32_t track(rc_cpu *c, uint32_t owner, uint32_t bar, int bar_index, uint32_t popup_menu, int32_t x, int32_t y,
                      uint32_t key, bool button_down)
{
    tracker t = {0};
    t.c = c;
    t.owner = owner;
    t.bar = bar;
    uint32_t hdc = rc_wnd_dc(0, true);
    t.d = rc_dc_get(hdc);
    if (!t.d) {
        return 0;
    }
    rc_wnd_send(c, owner, WM_ENTERMENULOOP, bar ? 0 : 1, 0);
    rc_wnd_send(c, owner, WM_INITMENU, bar ? rc_wnd_menu(bar) : popup_menu, 0);
    if (bar) {
        if (key) {
            menu *m = get(rc_wnd_menu(bar));
            for (int i = 0; m && i < m->n; i++) {
                const char *amp = m->items[i].text ? strchr(m->items[i].text, '&') : NULL;
                if (amp && (amp[1] | 0x20) == (int)(key | 0x20)) {
                    bar_index = i;
                }
            }
            if (bar_index < 0) {
                bar_index = 0;
            }
        }
        if (bar_index < 0) {
            bar_index = 0;
        }
        open_bar_item(&t, bar_index);
    } else {
        open_popup(&t, popup_menu, x, y, 0);
    }
    bool pressed = button_down;
    while (!t.done) {
        rc_event e;
        if (!rc_input_next_raw(&e)) {
            rc_desktop_present(false);
            rc_thread_sleep(1);
            continue;
        }
        if (e.type == RC_EV_QUIT) {
            rc_input_unread_raw(&e);
            break;
        }
        if (e.type == RC_EV_MOUSE_MOVE || e.type == RC_EV_MOUSE_BUTTON) {
            int level, index;
            bool over = at_item(&t, e.x, e.y, &level, &index);
            if (over) {
                close_to(&t, level + 1);
                select_item(&t, level, index);
                menu *m = get(t.stack[level].hmenu);
                if (index >= 0 && m && (m->items[index].flags & MF_POPUP) && t.depth == level + 1) {
                    rc_rect r = item_rect(&t.stack[level], index);
                    open_popup(&t, m->items[index].sub, r.r - 2, r.t - 2, index);
                }
                if (e.type == RC_EV_MOUSE_BUTTON && !e.down && index >= 0 && m && (pressed || e.button)) {
                    choose(&t, &m->items[index]);
                }
            } else if (bar) {
                rc_rect items[64];
                int n = bar_items(&t, items);
                for (int i = 0; i < n; i++) {
                    if (e.x >= items[i].l && e.x < items[i].r && e.y >= items[i].t && e.y < items[i].b) {
                        if (i != bar_hot) {
                            open_bar_item(&t, i);
                        }
                        over = true;
                    }
                }
            }
            if (e.type == RC_EV_MOUSE_BUTTON && e.down) {
                pressed = true;
                if (!over) {
                    break; /* a click elsewhere ends the menu */
                }
            }
            continue;
        }
        if (e.type == RC_EV_KEY && e.down) {
            popup *p = t.depth ? &t.stack[t.depth - 1] : NULL;
            menu *m = p ? get(p->hmenu) : NULL;
            switch (e.vk) {
            case 0x1b: /* Esc: one level back */
                if (t.depth > 1 || !bar) {
                    close_to(&t, t.depth - 1);
                    if (!t.depth) {
                        t.done = true;
                    }
                } else {
                    t.done = true;
                }
                break;
            case 0x26: /* up */
            case 0x28: /* down */
                if (m && m->n) {
                    int s = p->sel;
                    for (int k = 0; k < m->n; k++) {
                        s = (s + (e.vk == 0x28 ? 1 : m->n - 1)) % m->n;
                        if (!(m->items[s].flags & MF_SEPARATOR)) {
                            break;
                        }
                    }
                    select_item(&t, t.depth - 1, s);
                }
                break;
            case 0x25: /* left */
            case 0x27: /* right */
                if (e.vk == 0x27 && m && p->sel >= 0 && (m->items[p->sel].flags & MF_POPUP)) {
                    rc_rect r = item_rect(p, p->sel);
                    open_popup(&t, m->items[p->sel].sub, r.r - 2, r.t - 2, p->sel);
                } else if (e.vk == 0x25 && t.depth > 1) {
                    close_to(&t, t.depth - 1);
                } else if (bar) {
                    menu *bm = get(rc_wnd_menu(bar));
                    if (bm && bm->n) {
                        open_bar_item(&t, (bar_hot + (e.vk == 0x27 ? 1 : bm->n - 1)) % bm->n);
                    }
                }
                break;
            case 0x0d: /* Enter */
                if (m && p->sel >= 0) {
                    if (m->items[p->sel].flags & MF_POPUP) {
                        rc_rect r = item_rect(p, p->sel);
                        open_popup(&t, m->items[p->sel].sub, r.r - 2, r.t - 2, p->sel);
                    } else {
                        choose(&t, &m->items[p->sel]);
                    }
                }
                break;
            default:
                if (m && ((e.vk >= 'A' && e.vk <= 'Z') || (e.vk >= '0' && e.vk <= '9'))) {
                    for (int i = 0; i < m->n; i++) {
                        const char *amp = m->items[i].text ? strchr(m->items[i].text, '&') : NULL;
                        if (amp && (amp[1] & ~0x20) == (e.vk & ~0x20)) {
                            select_item(&t, t.depth - 1, i);
                            if (m->items[i].flags & MF_POPUP) {
                                rc_rect r = item_rect(p, i);
                                open_popup(&t, m->items[i].sub, r.r - 2, r.t - 2, i);
                            } else {
                                choose(&t, &m->items[i]);
                            }
                            break;
                        }
                    }
                }
                break;
            }
        }
    }
    close_to(&t, 0);
    bar_hot = -1;
    bar_hwnd = 0;
    redraw_bar(&t);
    rc_wnd_release_dc(hdc);
    rc_wnd_send(c, owner, WM_MENUSELECT, 0xffff0000u, 0);
    rc_wnd_send(c, owner, WM_EXITMENULOOP, bar ? 0 : 1, 0);
    rc_desktop_present(true);
    return t.chosen;
}

void rc_menu_bar_track(rc_cpu *c, uint32_t hwnd, int32_t x, int32_t y, uint32_t key)
{
    (void)y;
    if (!get(rc_wnd_menu(hwnd)) || !rc_desktop_active()) {
        return;
    }
    int index = -1;
    if (x >= 0) {
        tracker t = {0};
        uint32_t hdc = rc_wnd_dc(0, true);
        t.d = rc_dc_get(hdc);
        t.bar = hwnd;
        rc_rect items[64];
        int n = t.d ? bar_items(&t, items) : 0;
        rc_wnd_release_dc(hdc);
        for (int i = 0; i < n; i++) {
            if (x >= items[i].l && x < items[i].r) {
                index = i;
            }
        }
        if (index < 0) {
            return;
        }
    }
    uint32_t id = track(c, hwnd, hwnd, index, 0, 0, 0, key, x >= 0);
    if (id) {
        rc_post_message(hwnd, (id & 0xfff0u) >= 0xf000u ? WM_SYSCOMMAND : WM_COMMAND, id & 0xffff, 0);
    }
}

/* TrackPopupMenu(menu, flags, x, y, reserved, owner, rect) */
static void k_TrackPopupMenu(rc_cpu *c)
{
    uint32_t hmenu = RC_ARG(c, 0), flags = RC_ARG(c, 1), owner = RC_ARG(c, 5);
    if (!get(hmenu) || !rc_desktop_active()) {
        RC_RETURN(c, 0, 7);
    }
    int32_t x = (int32_t)RC_ARG(c, 2), y = (int32_t)RC_ARG(c, 3);
    uint32_t id = track(c, owner, 0, -1, hmenu, x, y, 0, false);
    if (id && !(flags & 0x100 /* TPM_RETURNCMD */)) {
        rc_post_message(owner, WM_COMMAND, id & 0xffff, 0);
        id = 1;
    }
    RC_RETURN(c, id, 7);
}

/* TrackPopupMenuEx(menu, flags, x, y, owner, params) */
static void k_TrackPopupMenuEx(rc_cpu *c)
{
    uint32_t hmenu = RC_ARG(c, 0), flags = RC_ARG(c, 1), owner = RC_ARG(c, 4);
    if (!get(hmenu) || !rc_desktop_active()) {
        RC_RETURN(c, 0, 6);
    }
    uint32_t id = track(c, owner, 0, -1, hmenu, (int32_t)RC_ARG(c, 2), (int32_t)RC_ARG(c, 3), 0, false);
    if (id && !(flags & 0x100)) {
        rc_post_message(owner, WM_COMMAND, id & 0xffff, 0);
        id = 1;
    }
    RC_RETURN(c, id, 6);
}

static void k_EndMenu(rc_cpu *c) { RC_RETURN(c, 1, 0); }

const rc_import_def rc_user32_menu[] = {
    {U32, "LoadMenuA", k_LoadMenuA},
    {U32, "LoadMenuIndirectA", k_LoadMenuIndirectA},
    {U32, "CreateMenu", k_CreateMenu},
    {U32, "CreatePopupMenu", k_CreatePopupMenu},
    {U32, "DestroyMenu", k_DestroyMenu},
    {U32, "IsMenu", k_IsMenu},
    {U32, "AppendMenuA", k_AppendMenuA},
    {U32, "InsertMenuA", k_InsertMenuA},
    {U32, "ModifyMenuA", k_ModifyMenuA},
    {U32, "DeleteMenu", k_DeleteMenu},
    {U32, "RemoveMenu", k_RemoveMenu},
    {U32, "GetSubMenu", k_GetSubMenu},
    {U32, "GetMenuItemCount", k_GetMenuItemCount},
    {U32, "GetMenuItemID", k_GetMenuItemID},
    {U32, "GetMenuState", k_GetMenuState},
    {U32, "GetMenuStringA", k_GetMenuStringA},
    {U32, "CheckMenuItem", k_CheckMenuItem},
    {U32, "EnableMenuItem", k_EnableMenuItem},
    {U32, "CheckMenuRadioItem", k_CheckMenuRadioItem},
    {U32, "GetMenuCheckMarkDimensions", k_GetMenuCheckMarkDimensions},
    {U32, "SetMenuItemBitmaps", k_SetMenuItemBitmaps},
    {U32, "HiliteMenuItem", k_HiliteMenuItem},
    {U32, "SetMenuDefaultItem", k_SetMenuDefaultItem},
    {U32, "GetMenuDefaultItem", k_GetMenuDefaultItem},
    {U32, "GetSystemMenu", k_GetSystemMenu},
    {U32, "GetMenuItemInfoA", k_GetMenuItemInfoA},
    {U32, "SetMenuItemInfoA", k_SetMenuItemInfoA},
    {U32, "TrackPopupMenu", k_TrackPopupMenu},
    {U32, "TrackPopupMenuEx", k_TrackPopupMenuEx},
    {U32, "EndMenu", k_EndMenu},
};
const uint32_t rc_user32_menu_count = sizeof rc_user32_menu / sizeof rc_user32_menu[0];

void rc_menu_init(void) { rc_register_imports(rc_user32_menu, rc_user32_menu_count); }
