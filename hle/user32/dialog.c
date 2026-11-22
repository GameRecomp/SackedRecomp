/* SPDX-License-Identifier: MIT */
/*
 * user32: dialogs. Dialog templates (DLGTEMPLATE and DLGTEMPLATEEX) become a window of the
 * dialog class (#32770) with its controls, sized in dialog units (the base units of MS Sans
 * Serif 8: 6 by 13 pixels). The dialog class's window procedure is DefDlgProc, which calls the
 * program's dialog procedure (DWL_DLGPROC) first. IsDialogMessage gives dialogs their keyboard:
 * Tab, Enter, Escape and the arrows in groups. Message boxes are dialogs made here (on our
 * desktop; a game's are answered without being shown, as before).
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
#include "rc/process.h"
#include "rc/text.h"
#include "rc/vm.h"
#include "user32.h"

#define U32 "USER32.dll"

#define WM_DESTROY 0x0002u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_CLOSE 0x0010u
#define WM_ERASEBKGND 0x0014u
#define WM_SETFONT 0x0030u
#define WM_NEXTDLGCTL 0x0028u
#define WM_GETDLGCODE 0x0087u
#define WM_KEYDOWN 0x0100u
#define WM_CHAR 0x0102u
#define WM_SYSCHAR 0x0106u
#define WM_INITDIALOG 0x0110u
#define WM_COMMAND 0x0111u
#define WM_CTLCOLORMSGBOX 0x0132u
#define WM_CTLCOLORSTATIC 0x0138u
#define DM_GETDEFID 0x0400u
#define DM_SETDEFID 0x0401u

#define DS_SETFONT 0x40u
#define DS_MODALFRAME 0x80u
#define DS_CENTER 0x800u
#define DS_CONTROL 0x400u

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

#define DWL_MSGRESULT 0
#define DWL_DLGPROC 4
#define DWL_USER 8

static void ret(rc_cpu *c, uint32_t v, uint32_t n) { RC_RETURN(c, v, n); }

/* A dialog's state on our side. */
typedef struct dialog {
    uint32_t defid, focus, result, msgbox;
    bool ended;
} dialog;

static dialog *dlg(uint32_t hwnd)
{
    dialog *d = rc_wnd_data(hwnd);
    if (!d && rc_wnd_ok(hwnd)) {
        d = calloc(1, sizeof *d);
        d->defid = 1; /* IDOK */
        rc_wnd_set_data(hwnd, d);
    }
    return d;
}

static uint32_t get_long(rc_cpu *c, uint32_t hwnd, int32_t index)
{
    (void)c;
    uint32_t v = 0;
    /* the window's extra bytes, through GetWindowLongA's view */
    v = rc_wnd_long(hwnd, index);
    return v;
}

/* ---- DefDlgProc ---- */

static uint32_t first_tab(uint32_t dlgw)
{
    for (uint32_t k = rc_wnd_first_child(dlgw); k; k = rc_wnd_next_sibling(k)) {
        uint32_t s = rc_wnd_style(k);
        if ((s & WS_TABSTOP) && (s & WS_VISIBLE) && !(s & WS_DISABLED)) {
            return k;
        }
    }
    return 0;
}

static uint32_t item_by_id(uint32_t dlgw, uint32_t id)
{
    for (uint32_t k = rc_wnd_first_child(dlgw); k; k = rc_wnd_next_sibling(k)) {
        if (rc_wnd_id(k) == id) {
            return k;
        }
    }
    return 0;
}

static void set_focus_ctl(rc_cpu *c, uint32_t ctl)
{
    if (!ctl) {
        return;
    }
    rc_wnd_set_focus(c, ctl);
    if (rc_wnd_send(c, ctl, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL) {
        rc_wnd_send(c, ctl, 0x00b1 /* EM_SETSEL */, 0, 0xffffffffu);
    }
}

static uint32_t def_dlg(rc_cpu *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    dialog *d = dlg(hwnd);
    uint32_t proc = get_long(c, hwnd, DWL_DLGPROC);
    if (proc) {
        uint32_t args[4] = {hwnd, msg, wp, lp};
        uint32_t r = rc_call_guest(c, proc, 4, args);
        if (!rc_wnd_ok(hwnd)) {
            return r;
        }
        if (msg == WM_INITDIALOG || (msg >= WM_CTLCOLORMSGBOX && msg <= WM_CTLCOLORSTATIC) || msg == 0x37 /* QUERYDRAGICON */ ||
            msg == 0x39 /* COMPAREITEM */ || msg == 0x2e || msg == 0x2f) {
            if (r || msg == WM_INITDIALOG) {
                return r;
            }
        } else if (r) {
            return get_long(c, hwnd, DWL_MSGRESULT);
        }
    }
    switch (msg) {
    case WM_INITDIALOG:
        return 1;
    case WM_ERASEBKGND: {
        rc_dc *dc = rc_dc_get(wp);
        if (dc) {
            rc_dc_fill(dc, rc_wnd_client(hwnd), rc_sys_color(15));
        }
        return 1;
    }
    case WM_CLOSE:
        if (d && d->msgbox) {
            d->result = 2;
            d->ended = true;
            return 0;
        }
        rc_post_message(hwnd, WM_COMMAND, 2 /* IDCANCEL */, item_by_id(hwnd, 2));
        return 0;
    case WM_COMMAND:
        if (d && d->msgbox && (wp & 0xffff) >= 1 && (wp & 0xffff) <= 7) {
            d->result = wp & 0xffff;
            d->ended = true;
        }
        return 0;
    case DM_GETDEFID:
        return d ? (0x534bu << 16) | (d->defid & 0xffff) : 0;
    case DM_SETDEFID:
        if (d) {
            d->defid = wp;
        }
        return 1;
    case WM_NEXTDLGCTL:
        if (lp & 0xffff) {
            set_focus_ctl(c, wp);
        } else {
            set_focus_ctl(c, rc_next_tab(c, hwnd, rc_wnd_focus(), wp != 0));
        }
        return 0;
    case WM_ACTIVATE:
        if (d) {
            if ((wp & 0xffff) == 0) {
                d->focus = rc_wnd_focus();
            } else if (d->focus && rc_wnd_ok(d->focus)) {
                rc_wnd_set_focus(c, d->focus);
            } else {
                set_focus_ctl(c, first_tab(hwnd));
            }
        }
        return 0;
    case WM_SETFOCUS: {
        uint32_t f = d && d->focus && rc_wnd_ok(d->focus) ? d->focus : first_tab(hwnd);
        if (f) {
            set_focus_ctl(c, f);
        }
        return 0;
    }
    case WM_GETDLGCODE:
        return 0;
    default:
        if (msg >= WM_CTLCOLORMSGBOX && msg <= WM_CTLCOLORSTATIC) {
            return rc_def_window_proc(c, hwnd, msg, wp, lp);
        }
        return rc_def_window_proc(c, hwnd, msg, wp, lp);
    }
}

static void k_DefDlgProcA(rc_cpu *c) { ret(c, def_dlg(c, RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3)), 4); }

/* The dialog class's window procedure: DefDlgProc. */
static void wp_dialog(rc_cpu *c) { k_DefDlgProcA(c); }

/* ---- templates ---- */

typedef struct reader {
    uint32_t p;
} reader;

static uint16_t rd16(reader *r)
{
    uint16_t v = rc_ld16(r->p);
    r->p += 2;
    return v;
}
static uint32_t rd32(reader *r)
{
    uint32_t v = rc_ld32(r->p);
    r->p += 4;
    return v;
}

/* A UTF-16 string into code page 1252 text. */
static void rd_wstr(reader *r, char *out, size_t cap)
{
    size_t k = 0;
    for (uint16_t ch; (ch = rd16(r)) != 0;) {
        if (k + 1 < cap) {
            out[k++] = (char)(ch < 256 ? ch : '?');
        }
    }
    out[k] = 0;
}

/* A name or ordinal: an ordinal gives "#n" (or *ord). */
static void rd_name(reader *r, char *out, size_t cap, uint32_t *ord)
{
    uint16_t first = rc_ld16(r->p);
    *ord = 0;
    if (first == 0) {
        r->p += 2;
        out[0] = 0;
    } else if (first == 0xffff) {
        r->p += 2;
        *ord = rd16(r);
        snprintf(out, cap, "#%u", *ord);
    } else {
        rd_wstr(r, out, cap);
    }
}

static void align4(reader *r) { r->p = (r->p + 3) & ~3u; }

static int32_t dlu_x(int32_t v) { return v * 6 / 4; }
static int32_t dlu_y(int32_t v) { return v * 13 / 8; }

/* The window rectangle around a client area for a style (as AdjustWindowRectEx). */
static void adjust(int32_t *w, int32_t *h, uint32_t style, uint32_t exstyle)
{
    int32_t b = (style & WS_THICKFRAME) ? 4 : ((style & WS_DLGFRAME) || (exstyle & 1)) ? 3 : (style & WS_BORDER) ? 1 : 0;
    *w += 2 * b;
    *h += 2 * b + ((style & WS_CAPTION) == WS_CAPTION ? 19 : 0);
    if (exstyle & WS_EX_CLIENTEDGE) {
        *w += 4;
        *h += 4;
    }
}

static uint32_t dialog_font(const char *face, int32_t points, uint32_t weight, bool italic)
{
    uint32_t h = rc_gobj_new(OB_FONT);
    gobj *f = rc_gobj_get(h, OB_FONT);
    snprintf(f->face, sizeof f->face, "%s", face && *face ? face : "MS Sans Serif");
    f->height = -(points * 96 + 36) / 72;
    f->weight = weight ? weight : 400;
    f->italic = italic;
    return h;
}

static const char *class_of(uint32_t ord, const char *name)
{
    switch (ord) {
    case 0x80: return "Button";
    case 0x81: return "Edit";
    case 0x82: return "Static";
    case 0x83: return "ListBox";
    case 0x84: return "ScrollBar";
    case 0x85: return "ComboBox";
    default: return name;
    }
}

/* Make a dialog from its template (the window and its controls), and send WM_INITDIALOG. */
static uint32_t create_dialog(rc_cpu *c, uint32_t inst, uint32_t tmpl, uint32_t owner, uint32_t proc, uint32_t param)
{
    reader r = {tmpl};
    bool ex = rc_ld16(tmpl) == 1 && rc_ld16(tmpl + 2) == 0xffff;
    uint32_t style, exstyle;
    if (ex) {
        r.p += 8; /* version, signature, help id */
        exstyle = rd32(&r);
        style = rd32(&r);
    } else {
        style = rd32(&r);
        exstyle = rd32(&r);
    }
    uint32_t count = rd16(&r);
    int32_t x = (int16_t)rd16(&r), y = (int16_t)rd16(&r), cx = (int16_t)rd16(&r), cy = (int16_t)rd16(&r);
    char menu[64], cls[64], title[256], face[64] = "MS Sans Serif";
    uint32_t menu_ord, cls_ord;
    rd_name(&r, menu, sizeof menu, &menu_ord);
    rd_name(&r, cls, sizeof cls, &cls_ord);
    rd_wstr(&r, title, sizeof title);
    uint32_t font = 0;
    if (style & DS_SETFONT) {
        int32_t points = rd16(&r);
        uint32_t weight = 400;
        bool italic = false;
        if (ex) {
            weight = rd16(&r);
            italic = rc_ld8(r.p) != 0;
            r.p += 2;
        }
        rd_wstr(&r, face, sizeof face);
        font = dialog_font(face, points, weight, italic);
    } else {
        font = rc_gui_font();
    }
    int32_t w = dlu_x(cx), h = dlu_y(cy);
    if (style & DS_MODALFRAME) {
        exstyle |= WS_EX_DLGMODALFRAME;
    }
    adjust(&w, &h, style, exstyle);
    int32_t px = dlu_x(x), py = dlu_y(y);
    if (!(style & WS_CHILD) && owner && rc_wnd_ok(owner)) {
        int32_t ox, oy;
        rc_wnd_client_origin(owner, &ox, &oy);
        px += ox;
        py += oy;
    }
    if ((style & DS_CENTER) && !(style & WS_CHILD)) {
        int32_t dw, dh;
        rc_desktop_size(&dw, &dh);
        px = (dw - w) / 2;
        py = (dh - h) / 2;
    }
    const char *class_name = cls[0] ? cls : "#32770";
    uint32_t hwnd = rc_wnd_create(c, exstyle, class_name, title, style & ~WS_VISIBLE, px, py, w, h, owner,
                                  menu_ord ? 0 : 0, inst, param);
    if (!hwnd) {
        return 0;
    }
    rc_wnd_set_long(hwnd, DWL_DLGPROC, proc);
    dlg(hwnd);
    rc_wnd_send(c, hwnd, WM_SETFONT, font, 0);
    for (uint32_t i = 0; i < count && rc_wnd_ok(hwnd); i++) {
        align4(&r);
        uint32_t istyle, iexstyle, id;
        if (ex) {
            r.p += 4; /* help id */
            iexstyle = rd32(&r);
            istyle = rd32(&r);
        } else {
            istyle = rd32(&r);
            iexstyle = rd32(&r);
        }
        int32_t ix = (int16_t)rd16(&r), iy = (int16_t)rd16(&r), icx = (int16_t)rd16(&r), icy = (int16_t)rd16(&r);
        id = ex ? rd32(&r) : rd16(&r);
        char icls[64], itext[512];
        uint32_t icls_ord, itext_ord;
        rd_name(&r, icls, sizeof icls, &icls_ord);
        rd_name(&r, itext, sizeof itext, &itext_ord);
        uint32_t extra = rd16(&r), data = 0;
        if (extra) {
            data = r.p;
            r.p += extra;
        }
        if (itext_ord) {
            itext[0] = 0; /* an icon or bitmap resource by number */
        }
        const char *k = class_of(icls_ord, icls);
        uint32_t ctl = rc_wnd_create(c, iexstyle | 0x4 /* NOPARENTNOTIFY */, k, itext, istyle | WS_CHILD, dlu_x(ix), dlu_y(iy),
                                     dlu_x(icx), dlu_y(icy), hwnd, id, inst, data);
        if (ctl) {
            rc_wnd_send(c, ctl, WM_SETFONT, font, 0);
            if (!strcmp(k, "Button") && (istyle & 0xf) == 1) {
                dlg(hwnd)->defid = id;
            }
        }
    }
    if (!rc_wnd_ok(hwnd)) {
        return 0;
    }
    uint32_t first = first_tab(hwnd);
    uint32_t focus = def_dlg(c, hwnd, WM_INITDIALOG, first, param);
    if (!rc_wnd_ok(hwnd)) {
        return 0;
    }
    if (focus && first) {
        set_focus_ctl(c, first);
    }
    if (style & WS_VISIBLE) {
        rc_wnd_show(c, hwnd, 5);
    }
    return hwnd;
}

static uint32_t find_template(uint32_t inst, uint32_t name)
{
    uint32_t data, size;
    bool found = name < 0x10000 ? rc_resource(inst, 5, name, &data, &size)
                                : rc_resource_named(inst, 5, rc_str(name), &data, &size);
    return found ? data : 0;
}

/* CreateDialogParamA(instance, name, owner, proc, param) */
static void k_CreateDialogParamA(rc_cpu *c)
{
    uint32_t t = find_template(RC_ARG(c, 0), RC_ARG(c, 1));
    ret(c, t ? create_dialog(c, RC_ARG(c, 0), t, RC_ARG(c, 2), RC_ARG(c, 3), RC_ARG(c, 4)) : 0, 5);
}

/* CreateDialogIndirectParamA(instance, template, owner, proc, param) */
static void k_CreateDialogIndirectParamA(rc_cpu *c)
{
    ret(c, create_dialog(c, RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3), RC_ARG(c, 4)), 5);
}

static uint32_t run_modal(rc_cpu *c, uint32_t hwnd, uint32_t owner)
{
    if (!hwnd) {
        return 0xffffffffu;
    }
    if (owner && rc_wnd_ok(owner)) {
        rc_wnd_enable(c, rc_wnd_top_level(owner), false);
    }
    rc_wnd_show(c, hwnd, 5);
    dialog *d = dlg(hwnd);
    rc_modal_loop(c, hwnd, &d->ended);
    uint32_t result = rc_wnd_ok(hwnd) && rc_wnd_data(hwnd) == d ? d->result : 0;
    if (owner && rc_wnd_ok(owner)) {
        rc_wnd_enable(c, rc_wnd_top_level(owner), true);
        rc_wnd_activate(c, rc_wnd_top_level(owner));
    }
    if (rc_wnd_ok(hwnd)) {
        rc_wnd_destroy(c, hwnd);
    }
    return result;
}

/* DialogBoxParamA(instance, name, owner, proc, param) */
static void k_DialogBoxParamA(rc_cpu *c)
{
    uint32_t t = find_template(RC_ARG(c, 0), RC_ARG(c, 1));
    uint32_t hwnd = t ? create_dialog(c, RC_ARG(c, 0), t, RC_ARG(c, 2), RC_ARG(c, 3), RC_ARG(c, 4)) : 0;
    ret(c, run_modal(c, hwnd, RC_ARG(c, 2)), 5);
}

static void k_DialogBoxIndirectParamA(rc_cpu *c)
{
    uint32_t hwnd = create_dialog(c, RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3), RC_ARG(c, 4));
    ret(c, run_modal(c, hwnd, RC_ARG(c, 2)), 5);
}

/* EndDialog(hwnd, result): a modal loop of ours ends; the window is hidden. */
static void k_EndDialog(rc_cpu *c)
{
    dialog *d = dlg(RC_ARG(c, 0));
    if (!d) {
        ret(c, 0, 2);
        return;
    }
    d->result = RC_ARG(c, 1);
    d->ended = true;
    rc_wnd_show(c, RC_ARG(c, 0), 0);
    ret(c, 1, 2);
}

/* ---- keyboard ---- */

/* The next (or previous) control with WS_TABSTOP, wrapping around. */
uint32_t rc_next_tab(rc_cpu *c, uint32_t dlgw, uint32_t from, bool back)
{
    (void)c;
    uint32_t list[256];
    int n = 0, at = -1;
    for (uint32_t k = rc_wnd_first_child(dlgw); k && n < 256; k = rc_wnd_next_sibling(k)) {
        if (k == from) {
            at = n;
        }
        list[n++] = k;
    }
    for (int i = 1; i <= n; i++) {
        int j = ((at < 0 ? (back ? n : -1) : at) + (back ? -i : i) + 2 * n) % n;
        uint32_t s = rc_wnd_style(list[j]);
        if ((s & WS_TABSTOP) && (s & WS_VISIBLE) && !(s & WS_DISABLED)) {
            return list[j];
        }
    }
    return from;
}

/* The next control in the group (arrows): up to the next WS_GROUP. */
static uint32_t next_in_group(uint32_t dlgw, uint32_t from, bool back)
{
    uint32_t list[256];
    int n = 0, at = -1;
    for (uint32_t k = rc_wnd_first_child(dlgw); k && n < 256; k = rc_wnd_next_sibling(k)) {
        if (k == from) {
            at = n;
        }
        list[n++] = k;
    }
    if (at < 0) {
        return from;
    }
    int start = at;
    while (start > 0 && !(rc_wnd_style(list[start]) & WS_GROUP)) {
        start--;
    }
    int end = at + 1;
    while (end < n && !(rc_wnd_style(list[end]) & WS_GROUP)) {
        end++;
    }
    int len = end - start;
    for (int i = 1; i < len; i++) {
        int j = start + ((at - start) + (back ? -i : i) + len) % len;
        uint32_t s = rc_wnd_style(list[j]);
        if ((s & WS_VISIBLE) && !(s & WS_DISABLED)) {
            return list[j];
        }
    }
    return from;
}

static void press_button(rc_cpu *c, uint32_t dlgw, uint32_t id)
{
    uint32_t b = item_by_id(dlgw, id);
    if (b && !rc_wnd_enabled(b)) {
        return;
    }
    rc_wnd_send(c, dlgw, WM_COMMAND, id & 0xffff, b);
}

bool rc_is_dialog_message(rc_cpu *c, uint32_t dlgw, uint32_t m)
{
    uint32_t hwnd = rc_ld32(m), msg = rc_ld32(m + 4), wp = rc_ld32(m + 8);
    if (!rc_wnd_ok(dlgw) || (hwnd != dlgw && !(rc_wnd_top_level(hwnd) == rc_wnd_top_level(dlgw)))) {
        return false;
    }
    uint32_t code = rc_wnd_ok(hwnd) ? rc_wnd_send(c, hwnd, WM_GETDLGCODE, wp, m) : 0;
    if (msg == WM_KEYDOWN && !(code & DLGC_WANTALLKEYS)) {
        switch (wp) {
        case 0x09: /* Tab */
            if (!(code & DLGC_WANTTAB)) {
                set_focus_ctl(c, rc_next_tab(c, dlgw, rc_wnd_focus(), (rc_key_state(0x10) & 0x80) != 0));
                return true;
            }
            break;
        case 0x0d: { /* Enter: the focused push button, or the default one */
            uint32_t f = rc_wnd_focus();
            uint32_t fc = f ? rc_wnd_send(c, f, WM_GETDLGCODE, 0, 0) : 0;
            if (fc & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)) {
                press_button(c, dlgw, rc_wnd_id(f));
            } else {
                uint32_t def = rc_wnd_send(c, dlgw, DM_GETDEFID, 0, 0);
                press_button(c, dlgw, (def >> 16) == 0x534b ? def & 0xffff : 1);
            }
            return true;
        }
        case 0x1b: /* Escape */
            press_button(c, dlgw, 2);
            return true;
        case 0x25: case 0x26: case 0x27: case 0x28:
            if (!(code & DLGC_WANTARROWS)) {
                uint32_t next = next_in_group(dlgw, rc_wnd_focus(), wp == 0x25 || wp == 0x26);
                set_focus_ctl(c, next);
                if (rc_wnd_send(c, next, WM_GETDLGCODE, 0, 0) & DLGC_RADIOBUTTON) {
                    rc_wnd_send(c, next, 0x00f5 /* BM_CLICK */, 0, 0);
                }
                return true;
            }
            break;
        default:
            break;
        }
    }
    if (msg == WM_CHAR && (wp == 0x09 || wp == 0x0d || wp == 0x1b) && !(code & (DLGC_WANTALLKEYS | DLGC_WANTCHARS))) {
        return true;
    }
    if (rc_wnd_ok(hwnd)) {
        rc_translate_key(m); /* as TranslateMessage: the key's WM_CHAR comes next */
        rc_wnd_send(c, hwnd, msg, wp, rc_ld32(m + 12));
    }
    return true;
}

/* IsDialogMessageA(dialog, msg): keys translated here too (TranslateMessage's WM_CHAR). */
static void k_IsDialogMessageA(rc_cpu *c)
{
    uint32_t dlgw = RC_ARG(c, 0), m = RC_ARG(c, 1);
    uint32_t hwnd = rc_ld32(m);
    if (!rc_wnd_ok(dlgw) || (hwnd != dlgw && rc_wnd_top_level(hwnd) != rc_wnd_top_level(dlgw)) ||
        rc_wnd_top_level(dlgw) != rc_wnd_top_level(hwnd)) {
        ret(c, 0, 2);
        return;
    }
    if (rc_ld32(m + 4) == WM_KEYDOWN) {
        rc_translate_key(m);
    }
    ret(c, rc_is_dialog_message(c, dlgw, m), 2);
}

/* ---- items ---- */

static void k_GetDlgItem(rc_cpu *c) { ret(c, item_by_id(RC_ARG(c, 0), RC_ARG(c, 1)), 2); }

static void k_SetDlgItemTextA(rc_cpu *c)
{
    uint32_t k = item_by_id(RC_ARG(c, 0), RC_ARG(c, 1));
    if (k) {
        rc_wnd_send(c, k, 0x000c, 0, RC_ARG(c, 2));
    }
    ret(c, k != 0, 3);
}

static void k_GetDlgItemTextA(rc_cpu *c)
{
    uint32_t k = item_by_id(RC_ARG(c, 0), RC_ARG(c, 1));
    if (!k) {
        if (RC_ARG(c, 3)) {
            rc_st8(RC_ARG(c, 2), 0);
        }
        ret(c, 0, 4);
        return;
    }
    ret(c, rc_wnd_send(c, k, 0x000d, RC_ARG(c, 3), RC_ARG(c, 2)), 4);
}

/* GetDlgItemInt(dialog, id, ok*, signed) */
static void k_GetDlgItemInt(rc_cpu *c)
{
    uint32_t k = item_by_id(RC_ARG(c, 0), RC_ARG(c, 1));
    uint32_t buf = rc_heap_alloc(64, true);
    if (k) {
        rc_wnd_send(c, k, 0x000d, 64, buf);
    }
    const char *s = rc_str(buf);
    char *end;
    long v = strtol(s, &end, 10);
    bool ok = k && end != s;
    if (RC_ARG(c, 2)) {
        rc_st32(RC_ARG(c, 2), ok);
    }
    rc_heap_free(buf);
    ret(c, ok ? (uint32_t)v : 0, 4);
}

static void k_SetDlgItemInt(rc_cpu *c)
{
    char s[32];
    if (RC_ARG(c, 3)) {
        snprintf(s, sizeof s, "%d", (int32_t)RC_ARG(c, 2));
    } else {
        snprintf(s, sizeof s, "%u", RC_ARG(c, 2));
    }
    uint32_t k = item_by_id(RC_ARG(c, 0), RC_ARG(c, 1));
    if (k) {
        uint32_t g = rc_proc_str(s);
        rc_wnd_send(c, k, 0x000c, 0, g);
    }
    ret(c, k != 0, 4);
}

static void k_SendDlgItemMessageA(rc_cpu *c)
{
    uint32_t k = item_by_id(RC_ARG(c, 0), RC_ARG(c, 1));
    ret(c, k ? rc_wnd_send(c, k, RC_ARG(c, 2), RC_ARG(c, 3), RC_ARG(c, 4)) : 0, 5);
}

static void k_CheckDlgButton(rc_cpu *c)
{
    uint32_t k = item_by_id(RC_ARG(c, 0), RC_ARG(c, 1));
    if (k) {
        rc_wnd_send(c, k, 0x00f1, RC_ARG(c, 2), 0);
    }
    ret(c, k != 0, 3);
}

static void k_IsDlgButtonChecked(rc_cpu *c)
{
    uint32_t k = item_by_id(RC_ARG(c, 0), RC_ARG(c, 1));
    ret(c, k ? rc_wnd_send(c, k, 0x00f0, 0, 0) : 0, 2);
}

/* CheckRadioButton(dialog, first, last, check) */
static void k_CheckRadioButton(rc_cpu *c)
{
    for (uint32_t id = RC_ARG(c, 1); id <= RC_ARG(c, 2); id++) {
        uint32_t k = item_by_id(RC_ARG(c, 0), id);
        if (k) {
            rc_wnd_send(c, k, 0x00f1, id == RC_ARG(c, 3), 0);
        }
    }
    ret(c, 1, 4);
}

static void k_GetNextDlgTabItem(rc_cpu *c) { ret(c, rc_next_tab(c, RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2) != 0), 3); }
static void k_GetNextDlgGroupItem(rc_cpu *c) { ret(c, next_in_group(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2) != 0), 3); }

static void k_MapDialogRect(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 1);
    rc_st32(p, (uint32_t)dlu_x((int32_t)rc_ld32(p)));
    rc_st32(p + 4, (uint32_t)dlu_y((int32_t)rc_ld32(p + 4)));
    rc_st32(p + 8, (uint32_t)dlu_x((int32_t)rc_ld32(p + 8)));
    rc_st32(p + 12, (uint32_t)dlu_y((int32_t)rc_ld32(p + 12)));
    ret(c, 1, 2);
}

static void k_GetDialogBaseUnits(rc_cpu *c) { ret(c, (13u << 16) | 6u, 0); }

/* ---- dialogs made in code (message boxes, comdlg32) ---- */

uint32_t rc_dialog_create(rc_cpu *c, uint32_t owner, const char *title, int32_t cw, int32_t ch, uint32_t proc)
{
    int32_t w = cw, h = ch;
    uint32_t style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    adjust(&w, &h, style, WS_EX_DLGMODALFRAME);
    int32_t dw, dh;
    rc_desktop_size(&dw, &dh);
    uint32_t hwnd =
        rc_wnd_create(c, WS_EX_DLGMODALFRAME, "#32770", title, style, (dw - w) / 2, (dh - h) / 2, w, h, owner, 0, 0, 0);
    if (!hwnd) {
        return 0;
    }
    rc_wnd_set_long(hwnd, DWL_DLGPROC, proc);
    dlg(hwnd);
    rc_wnd_send(c, hwnd, WM_SETFONT, rc_gui_font(), 0);
    return hwnd;
}

uint32_t rc_dialog_item(rc_cpu *c, uint32_t hwnd, const char *cls, const char *text, uint32_t style, int32_t x, int32_t y,
                        int32_t w, int32_t h, uint32_t id)
{
    uint32_t k = rc_wnd_create(c, 0, cls, text, style | WS_CHILD | WS_VISIBLE, x, y, w, h, hwnd, id, 0, 0);
    if (k) {
        rc_wnd_send(c, k, WM_SETFONT, rc_gui_font(), 0);
        if (!strcmp(cls, "Button") && (style & 0xf) == 1) {
            dlg(hwnd)->defid = id;
        }
    }
    return k;
}

uint32_t rc_dialog_run(rc_cpu *c, uint32_t hwnd, uint32_t owner, uint32_t focus)
{
    if (!hwnd) {
        return 0;
    }
    if (owner && rc_wnd_ok(owner)) {
        rc_wnd_enable(c, rc_wnd_top_level(owner), false);
    }
    dialog *d = dlg(hwnd);
    rc_wnd_show(c, hwnd, 5);
    set_focus_ctl(c, focus ? focus : first_tab(hwnd));
    rc_modal_loop(c, hwnd, &d->ended);
    uint32_t result = rc_wnd_ok(hwnd) && rc_wnd_data(hwnd) == d ? d->result : 2;
    if (owner && rc_wnd_ok(owner)) {
        rc_wnd_enable(c, rc_wnd_top_level(owner), true);
        rc_wnd_activate(c, rc_wnd_top_level(owner));
    }
    if (rc_wnd_ok(hwnd)) {
        rc_wnd_destroy(c, hwnd);
    }
    return result;
}

void rc_dialog_end(rc_cpu *c, uint32_t hwnd, uint32_t result)
{
    dialog *d = dlg(hwnd);
    if (d) {
        d->result = result;
        d->ended = true;
        rc_wnd_show(c, hwnd, 0);
    }
}

/* ---- message boxes ---- */

static uint32_t message_box(rc_cpu *c, uint32_t owner, const char *text, const char *caption, uint32_t type)
{
    if (!rc_desktop_active()) {
        rc_log("MessageBoxA: [%s] %s", caption, text);
        /* MB_OK, MB_OKCANCEL: OK; MB_YESNO(CANCEL): yes; retry/ignore boxes: retry. */
        static const uint32_t answer[] = {1, 1, 4, 6, 6, 4};
        return (type & 0xf) < 6 ? answer[type & 0xf] : 1;
    }
    static const struct {
        int n;
        uint32_t ids[3];
        const char *labels[3];
    } sets[6] = {
        {1, {1}, {"OK"}},
        {2, {1, 2}, {"OK", "Cancel"}},
        {3, {3, 4, 5}, {"&Abort", "&Retry", "&Ignore"}},
        {3, {6, 7, 2}, {"&Yes", "&No", "Cancel"}},
        {2, {6, 7}, {"&Yes", "&No"}},
        {2, {4, 2}, {"&Retry", "Cancel"}},
    };
    int set = (type & 0xf) < 6 ? (int)(type & 0xf) : 0;
    /* the text's size */
    uint32_t hdc = rc_wnd_dc(0, true);
    rc_dc *d = rc_dc_get(hdc);
    rc_rect tr = {0, 0, 420, 0};
    if (d) {
        d->font = rc_gui_font();
        rc_draw_text(d, text, -1, &tr, 0x410 /* CALCRECT WORDBREAK */);
    }
    rc_wnd_release_dc(hdc);
    int32_t bw = 75, bh = 23, gap = 6, n = sets[set].n;
    int32_t cw = tr.r + 30 > n * (bw + gap) + 20 ? tr.r + 30 : n * (bw + gap) + 20;
    int32_t ch = tr.b + 20 + bh + 22;
    uint32_t hwnd = rc_dialog_create(c, owner, caption, cw, ch, 0);
    if (!hwnd) {
        return 1;
    }
    dialog *dd = dlg(hwnd);
    dd->msgbox = 1;
    rc_dialog_item(c, hwnd, "Static", text, 0x80 /* SS_NOPREFIX */, 15, 12, tr.r + 4, tr.b, 0xffff);
    int32_t x = (cw - n * (bw + gap) + gap) / 2;
    uint32_t first = 0;
    for (int i = 0; i < n; i++) {
        uint32_t b = rc_dialog_item(c, hwnd, "Button", sets[set].labels[i], WS_TABSTOP | (i == 0 ? 1u | WS_GROUP : 0u),
                                    x, tr.b + 24, bw, bh, sets[set].ids[i]);
        if (!first) {
            first = b;
        }
        x += bw + gap;
    }
    dd->defid = sets[set].ids[0];
    uint32_t result = rc_dialog_run(c, hwnd, owner, first);
    return result ? result : 1;
}

uint32_t rc_message_box(rc_cpu *c, uint32_t owner, const char *text, const char *caption, uint32_t type)
{
    return message_box(c, owner, text, caption, type);
}

/* MessageBoxA(owner, text, caption, type) */
static void k_MessageBoxA(rc_cpu *c)
{
    const char *text = RC_ARG(c, 1) ? rc_str(RC_ARG(c, 1)) : "", *caption = RC_ARG(c, 2) ? rc_str(RC_ARG(c, 2)) : "Error";
    ret(c, message_box(c, RC_ARG(c, 0), text, caption, RC_ARG(c, 3)), 4);
}

/* MessageBoxIndirectA(MSGBOXPARAMSA: size, owner, instance, text, caption, style, ...) */
static void k_MessageBoxIndirectA(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    const char *text = rc_ld32(p + 12) >= 0x10000 ? rc_str(rc_ld32(p + 12)) : "";
    const char *caption = rc_ld32(p + 16) >= 0x10000 ? rc_str(rc_ld32(p + 16)) : "";
    ret(c, message_box(c, rc_ld32(p + 4), text, caption, rc_ld32(p + 20)), 1);
}

static const rc_import_def defs[] = {
    {U32, "DefDlgProcA", k_DefDlgProcA},
    {U32, "CreateDialogParamA", k_CreateDialogParamA},
    {U32, "CreateDialogIndirectParamA", k_CreateDialogIndirectParamA},
    {U32, "DialogBoxParamA", k_DialogBoxParamA},
    {U32, "DialogBoxIndirectParamA", k_DialogBoxIndirectParamA},
    {U32, "EndDialog", k_EndDialog},
    {U32, "IsDialogMessageA", k_IsDialogMessageA},
    {U32, "IsDialogMessage", k_IsDialogMessageA},
    {U32, "GetDlgItem", k_GetDlgItem},
    {U32, "SetDlgItemTextA", k_SetDlgItemTextA},
    {U32, "GetDlgItemTextA", k_GetDlgItemTextA},
    {U32, "GetDlgItemInt", k_GetDlgItemInt},
    {U32, "SetDlgItemInt", k_SetDlgItemInt},
    {U32, "SendDlgItemMessageA", k_SendDlgItemMessageA},
    {U32, "CheckDlgButton", k_CheckDlgButton},
    {U32, "IsDlgButtonChecked", k_IsDlgButtonChecked},
    {U32, "CheckRadioButton", k_CheckRadioButton},
    {U32, "GetNextDlgTabItem", k_GetNextDlgTabItem},
    {U32, "GetNextDlgGroupItem", k_GetNextDlgGroupItem},
    {U32, "MapDialogRect", k_MapDialogRect},
    {U32, "GetDialogBaseUnits", k_GetDialogBaseUnits},
    {U32, "MessageBoxA", k_MessageBoxA},
    {U32, "MessageBoxIndirectA", k_MessageBoxIndirectA},
};

void rc_dialog_init(void)
{
    rc_register_imports(defs, sizeof defs / sizeof defs[0]);
    rc_class_builtin("#32770", 0x8 /* CS_DBLCLKS */ | 0x800 /* CS_SAVEBITS */, wp_dialog, 30, 16 /* COLOR_BTNFACE + 1 */,
                     rc_arrow_cursor());
}
