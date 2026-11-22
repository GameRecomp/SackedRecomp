/* SPDX-License-Identifier: MIT */
/*
 * user32: what a program does before its first window exists: resources
 * (cursors, icons, accelerators, strings), window classes, registered
 * messages, rectangles. Windows are in window.c, messages and input in
 * input.c, menus, dialogs and controls in their own files.
 */
#define _POSIX_C_SOURCE 200809L /* strdup */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc/hle.h"
#include "rc/log.h"
#include "rc/platform.h"
#include "rc/process.h"
#include "rc/vm.h"
#include "gdi32/gdi32.h"
#include "kernel32/k32.h"
#include "rc/hostfs.h"
#include "user32.h"

#define U32 "USER32.dll"

/* ---- window classes ---- */
#define CLASSES 128
static rc_wclass classes[CLASSES];
static uint32_t nclasses;

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) {
            return 0;
        }
    }
    return *a == *b;
}

rc_wclass *rc_class_find(const char *name)
{
    if (!name) {
        return NULL;
    }
    if (name[0] == '#') { /* by atom */
        uint32_t atom = (uint32_t)strtoul(name + 1, NULL, 10);
        for (uint32_t i = 0; i < nclasses; i++) {
            if (classes[i].atom == atom) {
                return &classes[i];
            }
        }
    }
    for (uint32_t i = 0; i < nclasses; i++) {
        if (ieq(classes[i].name, name)) {
            return &classes[i];
        }
    }
    return NULL;
}

const rc_wclass_info *rc_find_class(const char *name, uint32_t *wndproc, uint32_t *style)
{
    rc_wclass *k = rc_class_find(name);
    if (k) {
        *wndproc = k->wndproc;
        *style = k->style;
    }
    return k;
}

static rc_wclass *class_slot(const char *name)
{
    rc_wclass *k = rc_class_find(name);
    if (k) {
        return k; /* registered again: replaced */
    }
    if (nclasses == CLASSES) {
        return NULL;
    }
    k = &classes[nclasses];
    memset(k, 0, sizeof *k);
    k->atom = 0xc000u + nclasses;
    nclasses++;
    return k;
}

void rc_class_builtin(const char *name, uint32_t style, void (*fn)(rc_cpu *c), uint32_t wnd_extra, uint32_t background,
                      uint32_t cursor)
{
    char import[80];
    snprintf(import, sizeof import, "WndProc::%s", name);
    rc_import_def *def = malloc(sizeof *def); /* the registry keeps the pointers */
    char *proc = strdup(import);
    if (!def || !proc) {
        free(def);
        free(proc);
        return;
    }
    *def = (rc_import_def){"USER32_CLASS", proc, fn};
    rc_register_imports(def, 1);
    rc_wclass *k = class_slot(name);
    if (!k) {
        return;
    }
    snprintf(k->name, sizeof k->name, "%s", name);
    k->style = style;
    k->wndproc = rc_import_address("USER32_CLASS", def->name);
    k->wnd_extra = wnd_extra;
    k->background = background;
    k->cursor = cursor;
}

/* WNDCLASSA, or WNDCLASSEXA from its second field on (`p` past cbSize; `small` its icon). */
static uint32_t register_class(uint32_t p, uint32_t small)
{
    uint32_t name = rc_ld32(p + 36);
    char text[64];
    if (name < 0x10000) {
        snprintf(text, sizeof text, "#%u", name);
    } else {
        snprintf(text, sizeof text, "%s", rc_str(name));
    }
    rc_wclass *w = class_slot(text);
    if (!w) {
        return 0;
    }
    snprintf(w->name, sizeof w->name, "%s", text);
    w->style = rc_ld32(p);
    w->wndproc = rc_ld32(p + 4);
    w->cls_extra = rc_ld32(p + 8);
    w->wnd_extra = rc_ld32(p + 12);
    w->instance = rc_ld32(p + 16);
    w->icon = rc_ld32(p + 20);
    w->cursor = rc_ld32(p + 24);
    w->background = rc_ld32(p + 28);
    w->menu_name = rc_ld32(p + 32);
    w->icon_small = small;
    return w->atom;
}

static void k_RegisterClassA(rc_cpu *c) { RC_RETURN(c, register_class(RC_ARG(c, 0), 0), 1); }

static void k_RegisterClassExA(rc_cpu *c)
{
    RC_RETURN(c, register_class(RC_ARG(c, 0) + 4, rc_ld32(RC_ARG(c, 0) + 44)), 1);
}

static rc_wclass *class_arg(uint32_t name)
{
    char text[64];
    if (name < 0x10000) {
        snprintf(text, sizeof text, "#%u", name);
    } else {
        snprintf(text, sizeof text, "%s", rc_str(name));
    }
    return rc_class_find(text);
}

/* GetClassInfoA(instance, name, WNDCLASSA) */
static void k_GetClassInfoA(rc_cpu *c)
{
    rc_wclass *k = class_arg(RC_ARG(c, 1));
    uint32_t p = RC_ARG(c, 2);
    if (!k) {
        rc_last_error = 1411; /* ERROR_CLASS_DOES_NOT_EXIST */
        RC_RETURN(c, 0, 3);
    }
    uint32_t v[9] = {k->style, k->wndproc, k->cls_extra, k->wnd_extra, k->instance, k->icon, k->cursor, k->background, k->menu_name};
    for (int i = 0; i < 9; i++) {
        rc_st32(p + 4 * (uint32_t)i, v[i]);
    }
    rc_st32(p + 36, RC_ARG(c, 1));
    RC_RETURN(c, k->atom, 3);
}

/* GetClassInfoExA(instance, name, WNDCLASSEXA) */
static void k_GetClassInfoExA(rc_cpu *c)
{
    rc_wclass *k = class_arg(RC_ARG(c, 1));
    uint32_t p = RC_ARG(c, 2);
    if (!k) {
        RC_RETURN(c, 0, 3);
    }
    uint32_t v[11] = {48, k->style, k->wndproc, k->cls_extra, k->wnd_extra, k->instance, k->icon, k->cursor, k->background, k->menu_name, RC_ARG(c, 1)};
    for (int i = 0; i < 11; i++) {
        rc_st32(p + 4 * (uint32_t)i, v[i]);
    }
    rc_st32(p + 44, k->icon_small);
    RC_RETURN(c, k->atom, 3);
}

static void k_UnregisterClassA(rc_cpu *c)
{
    rc_wclass *k = class_arg(RC_ARG(c, 0));
    if (k) {
        k->name[0] = 0; /* the slot stays: windows may still point at it */
        k->atom = 0;
    }
    RC_RETURN(c, k != NULL, 2);
}

/* ---- resources: handles only, for now ---- */
static uint32_t next_resource = 0x00010100u;


static void k_LoadIconA(rc_cpu *c)
{
    next_resource += 4;
    RC_RETURN(c, next_resource, 2);
}

/* ---- accelerators ----
 * A table is the program's RT_ACCELERATOR resource: entries of {u16 flags, u16 key, u16 command,
 * u16 pad}, the last flagged 0x80. The handle points to it. */
#define ACCELS 32
#define ACCEL_HANDLE 0x00090000u
static struct {
    uint32_t data, n;
} accels[ACCELS];
static int naccels;

static void k_LoadAcceleratorsA(rc_cpu *c)
{
    uint32_t module = RC_ARG(c, 0), name = RC_ARG(c, 1), data = 0, size = 0;
    bool found = name < 0x10000 ? rc_resource(module, 9, name, &data, &size)
                                : rc_resource_named(module, 9, rc_str(name), &data, &size);
    if (!found || naccels == ACCELS) {
        RC_RETURN(c, 0, 2);
    }
    for (int i = 0; i < naccels; i++) {
        if (accels[i].data == data) {
            RC_RETURN(c, ACCEL_HANDLE + 4u * (uint32_t)i, 2);
        }
    }
    uint32_t n = 0;
    while (n * 8 + 8 <= size) {
        n++;
        if (rc_ld16(data + (n - 1) * 8) & 0x80) {
            break;
        }
    }
    accels[naccels].data = data;
    accels[naccels].n = n;
    RC_RETURN(c, ACCEL_HANDLE + 4u * (uint32_t)naccels++, 2);
}

/* TranslateAcceleratorA(hwnd, table, msg): a key that matches an entry (with the same Shift,
 * Ctrl and Alt) becomes WM_COMMAND (high word 1) to `hwnd`. */
static void k_TranslateAcceleratorA(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), i = (RC_ARG(c, 1) - ACCEL_HANDLE) / 4, msg = RC_ARG(c, 2);
    uint32_t m = rc_ld32(msg + 4), key = rc_ld32(msg + 8);
    bool down = m == 0x100 || m == 0x104, chr = m == 0x102 || m == 0x106;
    if (i >= (uint32_t)naccels || !hwnd || (!down && !chr)) {
        RC_RETURN(c, 0, 3);
    }
    bool shift = rc_key_state(0x10) & 0x80, ctrl = rc_key_state(0x11) & 0x80, alt = rc_key_state(0x12) & 0x80;
    for (uint32_t e = 0; e < accels[i].n; e++) {
        uint32_t at = accels[i].data + e * 8, flags = rc_ld16(at);
        if ((flags & 1) ? !down : !chr) {
            continue; /* FVIRTKEY entries match key downs, the others characters */
        }
        if (rc_ld16(at + 2) != (key & 0xffff)) {
            continue;
        }
        if ((flags & 1) && (!!(flags & 4) != shift || !!(flags & 8) != ctrl)) {
            continue;
        }
        if (!!(flags & 0x10) != alt) {
            continue;
        }
        rc_wnd_send(c, hwnd, 0x111, 0x10000u | rc_ld16(at + 4), 0);
        RC_RETURN(c, 1, 3);
    }
    RC_RETURN(c, 0, 3);
}

/* ---- cursors ----
 * A cursor is one of the system's (LoadCursorA with a standard id), one of
 * the program's own resources (LoadCursorA with its module), or an image a
 * program made (CreateIconIndirect), shown by the platform over the window
 * as the mouse's own pointer. */
#define CURSORS 64
#define CURSOR_HANDLE 0x00030000u
typedef struct ucursor {
    bool used;
    int system; /* RC_CURSOR_*, or -1 for the image */
    uint32_t res; /* the program's cursor resource it was made from, or 0 */
    uint32_t *argb;
    int w, h, hx, hy;
} ucursor;
static ucursor cursors[CURSORS];

static uint32_t cursor_new(void)
{
    for (uint32_t i = 1; i < CURSORS; i++) {
        if (!cursors[i].used) {
            cursors[i] = (ucursor){.used = true, .system = RC_CURSOR_ARROW};
            return CURSOR_HANDLE + 4 * i;
        }
    }
    return 0;
}

static ucursor *cursor_get(uint32_t h)
{
    if (h < CURSOR_HANDLE || (h - CURSOR_HANDLE) % 4 || (h - CURSOR_HANDLE) / 4 >= CURSORS) {
        return NULL;
    }
    ucursor *u = &cursors[(h - CURSOR_HANDLE) / 4];
    return u->used ? u : NULL;
}

/* The standard cursors (IDC_*) as the system's. */
static int system_cursor(uint32_t id)
{
    switch (id) {
    case 32513: return RC_CURSOR_TEXT;        /* IDC_IBEAM */
    case 32514: return RC_CURSOR_WAIT;        /* IDC_WAIT */
    case 32515: return RC_CURSOR_CROSSHAIR;   /* IDC_CROSS */
    case 32642: return RC_CURSOR_SIZE_NWSE;   /* IDC_SIZENWSE */
    case 32643: return RC_CURSOR_SIZE_NESW;   /* IDC_SIZENESW */
    case 32644: return RC_CURSOR_SIZE_WE;     /* IDC_SIZEWE */
    case 32645: return RC_CURSOR_SIZE_NS;     /* IDC_SIZENS */
    case 32646: return RC_CURSOR_MOVE;        /* IDC_SIZEALL */
    case 32648: return RC_CURSOR_NOT_ALLOWED; /* IDC_NO */
    case 32649: return RC_CURSOR_POINTER;     /* IDC_HAND */
    case 32650: return RC_CURSOR_PROGRESS;    /* IDC_APPSTARTING */
    default: return RC_CURSOR_ARROW;          /* IDC_ARROW and the rest */
    }
}

/* A cursor image of the program's (RT_CURSOR): the hot spot (two 16-bit
 * words), then a bitmap as in a .ico file: a BITMAPINFOHEADER whose height
 * counts the colour image and the AND mask below it, the colour table, the
 * colour image (bottom row first) and the 1-bit AND mask (1: transparent). */
static uint32_t *cursor_image(uint32_t at, uint32_t size, int *w_out, int *h_out, int *hx, int *hy)
{
    if (size < 4 + 40) {
        return NULL;
    }
    *hx = (int)rc_ld16(at);
    *hy = (int)rc_ld16(at + 2);
    uint32_t bi = at + 4;
    uint32_t hs = rc_ld32(bi), w = rc_ld32(bi + 4), h2 = rc_ld32(bi + 8), bpp = rc_ld16(bi + 14);
    uint32_t used = rc_ld32(bi + 32), h = h2 / 2;
    if (w == 0 || h == 0 || w > 256 || h > 256 || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)) {
        return NULL;
    }
    uint32_t colours = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    uint32_t pal = bi + hs, bits = pal + 4 * colours;
    uint32_t stride = (w * bpp + 31) / 32 * 4, mstride = (w + 31) / 32 * 4;
    uint32_t mask = bits + stride * h;
    if (mask + mstride * h > at + size) {
        return NULL;
    }
    uint32_t *out = malloc((size_t)w * h * 4);
    if (!out) {
        return NULL;
    }
    bool alpha = false; /* a 32-bit image with its own alpha */
    for (uint32_t i = 0; bpp == 32 && i < w * h; i++) {
        alpha |= (rc_ld32(bits + 4 * i) >> 24) != 0;
    }
    for (uint32_t y = 0; y < h; y++) {
        uint32_t row = bits + (h - 1 - y) * stride, mrow = mask + (h - 1 - y) * mstride;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t rgb;
            if (bpp <= 8) {
                uint32_t bit = x * bpp, idx = (rc_ld8(row + bit / 8) >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1);
                uint32_t q = idx < colours ? rc_ld32(pal + 4 * idx) : 0;
                rgb = q & 0xffffffu;
            } else if (bpp == 24) {
                uint32_t p = row + 3 * x;
                rgb = (uint32_t)rc_ld8(p + 2) << 16 | (uint32_t)rc_ld8(p + 1) << 8 | rc_ld8(p);
            } else {
                uint32_t q = rc_ld32(row + 4 * x);
                rgb = q & 0xffffffu;
                if (alpha) {
                    out[y * w + x] = (q >> 24) ? q : 0;
                    continue;
                }
            }
            int and = (rc_ld8(mrow + x / 8) >> (7 - x % 8)) & 1;
            if (and) {
                /* The screen XORed with the image: transparent where that
                 * is (nearly) black, which barely changes the screen, and
                 * black where it would invert it. */
                int bright = ((rgb >> 16) & 0xff) + ((rgb >> 8) & 0xff) + (rgb & 0xff) > 3 * 64;
                out[y * w + x] = bright ? 0xff000000u : 0;
            } else {
                out[y * w + x] = 0xff000000u | rgb;
            }
        }
    }
    *w_out = (int)w;
    *h_out = (int)h;
    return out;
}

/* The program's cursor `name` (a string, or a number below 0x10000): the
 * image its group (RT_GROUP_CURSOR) lists with the most colours, at the
 * largest size up to 32 x 32 if it has one. */
static uint32_t own_cursor(uint32_t inst, uint32_t name)
{
    uint32_t grp, gsize;
    bool found;
    if (name < 0x10000u) {
        found = rc_resource(inst, 12, name, &grp, &gsize);
    } else {
        char s[64];
        uint32_t i = 0;
        for (; i < sizeof s - 1 && rc_ld8(name + i); i++) {
            s[i] = (char)rc_ld8(name + i);
        }
        s[i] = 0;
        found = rc_resource_named(inst, 12, s, &grp, &gsize);
    }
    if (!found || gsize < 6) {
        return 0;
    }
    for (uint32_t i = 1; i < CURSORS; i++) {
        if (cursors[i].used && cursors[i].res == grp) {
            return CURSOR_HANDLE + 4 * i;
        }
    }
    uint32_t n = rc_ld16(grp + 4), best = 0, best_score = 0;
    for (uint32_t i = 0; i < n && 6 + 14 * (i + 1) <= gsize; i++) {
        uint32_t e = grp + 6 + 14 * i;
        uint32_t w = rc_ld16(e), bits = rc_ld16(e + 6), id = rc_ld16(e + 12);
        uint32_t score = (w <= 32 ? 0x10000u : 0) + bits * 256 + (w & 0xff);
        if (score > best_score) {
            best_score = score;
            best = id;
        }
    }
    uint32_t img, isize;
    if (!best || !rc_resource(inst, 1, best, &img, &isize)) {
        return 0;
    }
    uint32_t h = cursor_new();
    ucursor *u = cursor_get(h);
    if (!u) {
        return 0;
    }
    u->argb = cursor_image(img, isize, &u->w, &u->h, &u->hx, &u->hy);
    if (!u->argb) {
        *u = (ucursor){0};
        return 0;
    }
    u->system = -1;
    u->res = grp;
    return h;
}

static void k_LoadCursorA(rc_cpu *c)
{
    uint32_t inst = RC_ARG(c, 0), name = RC_ARG(c, 1);
    if (inst) {
        uint32_t own = own_cursor(inst, name);
        if (own) {
            RC_RETURN(c, own, 2);
        }
    }
    /* A standard cursor, or one of the program's that is not there: the
     * system's (the arrow for those it has not). */
    int sys = inst == 0 && name < 0x10000u ? system_cursor(name) : RC_CURSOR_ARROW;
    for (uint32_t i = 1; i < CURSORS; i++) {
        if (cursors[i].used && !cursors[i].argb && cursors[i].system == sys) {
            RC_RETURN(c, CURSOR_HANDLE + 4 * i, 2);
        }
    }
    uint32_t h = cursor_new();
    if (h) {
        cursor_get(h)->system = sys;
    }
    RC_RETURN(c, h, 2);
}

/* LoadImageA for icons (1) and cursors (2). */
uint32_t rc_load_icon_or_cursor(rc_cpu *c, uint32_t inst, uint32_t name, uint32_t type)
{
    (void)c;
    if (type == 2) {
        uint32_t own = inst ? own_cursor(inst, name) : 0;
        if (own) {
            return own;
        }
        return rc_arrow_cursor();
    }
    next_resource += 4;
    return next_resource;
}

/* The first image of a .cur file in memory (an icon directory: type 2, one 16-byte entry
 * per image with its hot spot, size and offset), or of an animated cursor (.ani: a RIFF
 * "ACON" whose LIST "fram" holds .cur files as "icon" chunks); a cursor made from it. */
static uint32_t cursor_from_file(const uint8_t *f, uint32_t n)
{
    if (n >= 12 && memcmp(f, "RIFF", 4) == 0 && memcmp(f + 8, "ACON", 4) == 0) {
        for (uint32_t at = 12; at + 8 <= n;) {
            uint32_t len = (uint32_t)f[at + 4] | (uint32_t)f[at + 5] << 8 | (uint32_t)f[at + 6] << 16 |
                           (uint32_t)f[at + 7] << 24;
            if (memcmp(f + at, "LIST", 4) == 0) {
                at += 12; /* into the list: "LIST", size, type */
                continue;
            }
            if (memcmp(f + at, "icon", 4) == 0 && at + 8 + len <= n) {
                return cursor_from_file(f + at + 8, len);
            }
            at += 8 + len + (len & 1);
        }
        return 0;
    }
    if (n < 6 + 16 || f[2] != 2 || f[4] == 0) {
        return 0;
    }
    uint32_t size = (uint32_t)f[14] | (uint32_t)f[15] << 8 | (uint32_t)f[16] << 16 | (uint32_t)f[17] << 24;
    uint32_t off = (uint32_t)f[18] | (uint32_t)f[19] << 8 | (uint32_t)f[20] << 16 | (uint32_t)f[21] << 24;
    if (off >= n || size > n - off) {
        return 0;
    }
    /* As a resource: the hot spot, then the bitmap. */
    uint32_t buf = rc_heap_alloc(size + 4, false);
    if (!buf) {
        return 0;
    }
    rc_st16(buf, (uint16_t)(f[10] | f[11] << 8));
    rc_st16(buf + 2, (uint16_t)(f[12] | f[13] << 8));
    memcpy(rc_ptr(buf + 4), f + off, size);
    uint32_t h = cursor_new();
    ucursor *u = cursor_get(h);
    if (u) {
        u->argb = cursor_image(buf, size + 4, &u->w, &u->h, &u->hx, &u->hy);
        if (u->argb) {
            u->system = -1;
        } else {
            *u = (ucursor){0};
            h = 0;
        }
    }
    rc_heap_free(buf);
    return h;
}

/* LoadCursorFromFileA(path): a .cur or .ani file of the game's. */
static void k_LoadCursorFromFileA(rc_cpu *c)
{
    const char *path = rc_str(RC_ARG(c, 0));
    char rel[RC_PATH_MAX], host[RC_PATH_MAX];
    uint32_t h = 0;
    if (path && rc_k32_guest_rel(path, rel, sizeof rel) && rc_k32_find_existing(rel, host, NULL, NULL)) {
        uint32_t size = 0;
        uint8_t *data = rc_host_read(host, &size);
        if (data) {
            h = cursor_from_file(data, size);
            free(data);
        }
    }
    if (!h) {
        rc_log("LoadCursorFromFileA(\"%s\"): not a cursor the program can use", path ? path : "");
    }
    RC_RETURN(c, h, 1);
}

/* The clipboard: the program's own (not shared with the host), enough for copying and pasting
 * inside it (an editor's objects). Formats it registers get numbers from 0xC000 on. */
#define CLIP_FORMATS 32
static struct {
    uint32_t format, handle;
} clip[CLIP_FORMATS];
static uint32_t nclip;
static char clip_names[64][64];
static uint32_t nclip_names;

static void k_OpenClipboard(rc_cpu *c) { RC_RETURN(c, 1, 1); }
static void k_CloseClipboard(rc_cpu *c) { RC_RETURN(c, 1, 0); }
static void k_EmptyClipboard(rc_cpu *c)
{
    nclip = 0;
    RC_RETURN(c, 1, 0);
}

static uint32_t clip_find(uint32_t format)
{
    for (uint32_t i = 0; i < nclip; i++) {
        if (clip[i].format == format) {
            return i;
        }
    }
    return CLIP_FORMATS;
}

/* GetClipboardData(format), SetClipboardData(format, handle), IsClipboardFormatAvailable(format) */
static void k_GetClipboardData(rc_cpu *c)
{
    uint32_t i = clip_find(RC_ARG(c, 0));
    RC_RETURN(c, i < CLIP_FORMATS ? clip[i].handle : 0, 1);
}

static void k_SetClipboardData(rc_cpu *c)
{
    uint32_t format = RC_ARG(c, 0), handle = RC_ARG(c, 1), i = clip_find(format);
    if (i == CLIP_FORMATS) {
        if (nclip == CLIP_FORMATS) {
            RC_RETURN(c, 0, 2);
        }
        i = nclip++;
    }
    clip[i].format = format;
    clip[i].handle = handle;
    RC_RETURN(c, handle, 2);
}

static void k_IsClipboardFormatAvailable(rc_cpu *c) { RC_RETURN(c, clip_find(RC_ARG(c, 0)) < CLIP_FORMATS, 1); }

/* RegisterClipboardFormatA(name): the same number for the same name. */
static void k_RegisterClipboardFormatA(rc_cpu *c)
{
    const char *name = RC_ARG(c, 0) ? rc_str(RC_ARG(c, 0)) : "";
    for (uint32_t i = 0; i < nclip_names; i++) {
        if (strcmp(clip_names[i], name) == 0) {
            RC_RETURN(c, 0xc000u + i, 1);
        }
    }
    if (nclip_names == 64) {
        RC_RETURN(c, 0, 1);
    }
    snprintf(clip_names[nclip_names], sizeof clip_names[0], "%s", name);
    RC_RETURN(c, 0xc000u + nclip_names++, 1);
}

/* wsprintfA(buffer, format, ...): cdecl, so the caller removes the arguments. The
 * conversions Windows has: %c %d %i %u %x %X %s and %%, with the flags '-', '0' and '#',
 * a width, a precision, and 'l' or 'h'; at most 1024 characters. */
/* wsprintfA's work: the arguments from the stack (va == 0) or from an argument list. */
static uint32_t format(rc_cpu *c, uint32_t out, const char *f, uint32_t va)
{
    uint32_t arg = 2;
    char buf[1025];
    size_t n = 0;
    while (f && *f && n < 1024) {
        if (*f != '%') {
            buf[n++] = *f++;
            continue;
        }
        f++;
        if (*f == '%') {
            buf[n++] = *f++;
            continue;
        }
        char spec[16] = "%";
        size_t k = 1;
        while ((*f == '-' || *f == '0' || *f == '#') && k < 6) {
            spec[k++] = *f++;
        }
        while (*f >= '0' && *f <= '9' && k < 10) {
            spec[k++] = *f++;
        }
        if (*f == '.') {
            spec[k++] = *f++;
            while (*f >= '0' && *f <= '9' && k < 13) {
                spec[k++] = *f++;
            }
        }
        while (*f == 'l' || *f == 'h') {
            f++;
        }
        char conv = *f ? *f++ : 0;
        char piece[1100];
        uint32_t v = va ? rc_ld32(va + 4 * (arg - 2)) : RC_ARG(c, arg);
        switch (conv) {
        case 'd':
        case 'i':
            spec[k++] = 'd';
            snprintf(piece, sizeof piece, spec, (int32_t)v);
            arg++;
            break;
        case 'u':
        case 'x':
        case 'X':
            spec[k++] = conv;
            snprintf(piece, sizeof piece, spec, v);
            arg++;
            break;
        case 'c':
            spec[k++] = 'c';
            snprintf(piece, sizeof piece, spec, (int)(v & 0xff));
            arg++;
            break;
        case 's':
            spec[k++] = 's';
            snprintf(piece, sizeof piece, spec, v ? rc_str(v) : "(null)");
            arg++;
            break;
        default:
            piece[0] = 0;
            break;
        }
        for (const char *p = piece; *p && n < 1024; p++) {
            buf[n++] = *p;
        }
    }
    buf[n] = 0;
    memcpy(rc_ptr(out), buf, n + 1);
    return (uint32_t)n;
}

static void k_wsprintfA(rc_cpu *c) { RC_RETURN(c, format(c, RC_ARG(c, 0), rc_str(RC_ARG(c, 1)), 0), 0); }
static void k_wvsprintfA(rc_cpu *c) { RC_RETURN(c, format(c, RC_ARG(c, 0), rc_str(RC_ARG(c, 1)), RC_ARG(c, 2)), 3); }

/* The pixels of an icon's masks (ICONINFO): a monochrome cursor has an AND
 * mask over an XOR mask in one bitmap twice its height; a colour one has
 * an AND mask and a colour bitmap. Pixels that would invert the screen are
 * drawn black. */
static uint32_t *icon_pixels(uint32_t mask, uint32_t color, int *w_out, int *h_out)
{
    uint32_t mw, mh, mbpp, mwb, mpx;
    if (!rc_gdi_bitmap(mask, &mw, &mh, &mbpp, &mwb, &mpx) || mbpp != 1) {
        return NULL;
    }
    uint32_t cw = 0, ch = 0, cbpp = 0, cwb = 0, cpx = 0;
    if (color && (!rc_gdi_bitmap(color, &cw, &ch, &cbpp, &cwb, &cpx) || (cbpp != 24 && cbpp != 32))) {
        return NULL;
    }
    uint32_t w = mw, h = color ? mh : mh / 2;
    if (w == 0 || h == 0 || w > 256 || h > 256 || (color && (cw < w || ch < h))) {
        return NULL;
    }
    uint32_t *out = malloc((size_t)w * h * 4);
    if (!out) {
        return NULL;
    }
    const uint8_t *mb = rc_ptr(mpx);
    const uint8_t *cb = color ? rc_ptr(cpx) : NULL;
    bool alpha = false;
    for (uint32_t y = 0; cb && cbpp == 32 && y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            alpha |= cb[y * cwb + x * 4 + 3] != 0;
        }
    }
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            int and = (mb[y * mwb + x / 8] >> (7 - x % 8)) & 1;
            uint32_t px;
            if (cb) {
                const uint8_t *p = cb + y * cwb + x * (cbpp / 8);
                uint32_t a = alpha ? p[3] : and ? 0 : 0xff;
                px = a ? a << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0] : 0;
            } else {
                int xor = (mb[(y + h) * mwb + x / 8] >> (7 - x % 8)) & 1;
                px = and && !xor ? 0 : xor && !and ? 0xffffffffu : 0xff000000u;
            }
            out[y * w + x] = px;
        }
    }
    *w_out = (int)w;
    *h_out = (int)h;
    return out;
}

static void k_CreateIconIndirect(rc_cpu *c)
{
    uint32_t info = RC_ARG(c, 0);
    uint32_t h = cursor_new();
    ucursor *u = cursor_get(h);
    if (u) {
        u->argb = icon_pixels(rc_ld32(info + 12), rc_ld32(info + 16), &u->w, &u->h);
        if (u->argb) {
            u->system = -1;
            if (rc_ld32(info) == 0) { /* a cursor: its own hot spot */
                u->hx = (int)rc_ld32(info + 4);
                u->hy = (int)rc_ld32(info + 8);
            } else {
                u->hx = u->w / 2;
                u->hy = u->h / 2;
            }
        }
    }
    RC_RETURN(c, h, 1);
}

static void k_DestroyCursor(rc_cpu *c)
{
    ucursor *u = cursor_get(RC_ARG(c, 0));
    if (u && u->argb) {
        free(u->argb);
        *u = (ucursor){0};
    }
    RC_RETURN(c, 1, 1);
}

/* The cursor set (none at first: the window class's arrow) and the display
 * counter ShowCursor keeps; it is shown while the counter is not negative. */
static uint32_t cursor;
static bool cursor_set;
static int32_t cursor_count;
static int cursor_shown = -1;

static void cursor_update(void)
{
    ucursor *u = cursor_get(cursor);
    int show = cursor_count >= 0 && (cursor || !cursor_set);
    if (show) {
        if (u && u->argb) {
            rc_plat_cursor_image(u->argb, u->w, u->h, u->hx, u->hy);
        } else {
            rc_plat_cursor_system(u ? u->system : RC_CURSOR_ARROW);
        }
    }
    if (show != cursor_shown) {
        rc_plat_cursor_visible(show);
        cursor_shown = show;
    }
}

void rc_set_cursor(uint32_t h)
{
    uint32_t old = cursor;
    cursor = h;
    bool changed = cursor != old || !cursor_set;
    cursor_set = true;
    if (changed) {
        cursor_update();
    }
}

static void k_SetCursor(rc_cpu *c)
{
    uint32_t old = cursor;
    rc_set_cursor(RC_ARG(c, 0));
    RC_RETURN(c, old, 1);
}

/* The system's arrow (as LoadCursorA(0, IDC_ARROW) gives it). */
uint32_t rc_arrow_cursor(void)
{
    for (uint32_t i = 1; i < CURSORS; i++) {
        if (cursors[i].used && !cursors[i].argb && cursors[i].system == RC_CURSOR_ARROW) {
            return CURSOR_HANDLE + 4 * i;
        }
    }
    uint32_t h = cursor_new();
    if (h) {
        cursor_get(h)->system = RC_CURSOR_ARROW;
    }
    return h;
}

static void k_ShowCursor(rc_cpu *c)
{
    cursor_count += RC_ARG(c, 0) ? 1 : -1;
    cursor_update();
    RC_RETURN(c, (uint32_t)cursor_count, 1);
}

/* Strings come in blocks of 16, each a length and that many UTF-16 units. */
static void k_LoadStringA(rc_cpu *c)
{
    uint32_t inst = RC_ARG(c, 0), id = RC_ARG(c, 1), buf = RC_ARG(c, 2), n = RC_ARG(c, 3);
    uint32_t data, size;
    if (n == 0 || !rc_resource(inst, 6, (id >> 4) + 1, &data, &size)) {
        if (n) {
            rc_st8(buf, 0);
        }
        RC_RETURN(c, 0, 4);
    }
    uint32_t p = data;
    for (uint32_t i = 0; i < (id & 15); i++) {
        p += 2 + 2 * rc_ld16(p);
    }
    uint32_t len = rc_ld16(p);
    if (len > n - 1) {
        len = n - 1;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint16_t ch = rc_ld16(p + 2 + 2 * i);
        rc_st8(buf + i, (uint8_t)(ch < 0x100 ? ch : '?'));
    }
    rc_st8(buf + len, 0);
    RC_RETURN(c, len, 4);
}

/* Messages registered by name get values from 0xc000 up. */
#define REGISTERED 32
static char registered[REGISTERED][64];
static uint32_t nregistered;

static void k_RegisterWindowMessageA(rc_cpu *c)
{
    const char *name = rc_str(RC_ARG(c, 0));
    for (uint32_t i = 0; i < nregistered; i++) {
        if (strcmp(registered[i], name) == 0) {
            RC_RETURN(c, 0xc100u + i, 1);
        }
    }
    if (nregistered == REGISTERED) {
        RC_RETURN(c, 0, 1);
    }
    snprintf(registered[nregistered], sizeof registered[0], "%s", name);
    RC_RETURN(c, 0xc100u + nregistered++, 1);
}

/* ---- rectangles ---- */
static void k_SetRect(rc_cpu *c)
{
    uint32_t r = RC_ARG(c, 0);
    for (uint32_t i = 0; i < 4; i++) {
        rc_st32(r + 4 * i, RC_ARG(c, 1 + i));
    }
    RC_RETURN(c, 1, 5);
}

static void k_OffsetRect(rc_cpu *c)
{
    uint32_t r = RC_ARG(c, 0), dx = RC_ARG(c, 1), dy = RC_ARG(c, 2);
    rc_st32(r, rc_ld32(r) + dx);
    rc_st32(r + 4, rc_ld32(r + 4) + dy);
    rc_st32(r + 8, rc_ld32(r + 8) + dx);
    rc_st32(r + 12, rc_ld32(r + 12) + dy);
    RC_RETURN(c, 1, 3);
}

/* RECT { left, top, right, bottom }. */
static int32_t rl(uint32_t r, int i) { return (int32_t)rc_ld32(r + 4u * (uint32_t)i); }
static void rs(uint32_t r, int32_t l, int32_t t, int32_t rt, int32_t b)
{
    rc_st32(r, (uint32_t)l);
    rc_st32(r + 4, (uint32_t)t);
    rc_st32(r + 8, (uint32_t)rt);
    rc_st32(r + 12, (uint32_t)b);
}
static bool empty(uint32_t r) { return rl(r, 2) <= rl(r, 0) || rl(r, 3) <= rl(r, 1); }
static int32_t mn(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t mx(int32_t a, int32_t b) { return a > b ? a : b; }

static void k_SetRectEmpty(rc_cpu *c)
{
    rs(RC_ARG(c, 0), 0, 0, 0, 0);
    RC_RETURN(c, 1, 1);
}

static void k_CopyRect(rc_cpu *c)
{
    uint32_t d = RC_ARG(c, 0), s = RC_ARG(c, 1);
    if (!d || !s) {
        RC_RETURN(c, 0, 2);
    }
    rs(d, rl(s, 0), rl(s, 1), rl(s, 2), rl(s, 3));
    RC_RETURN(c, 1, 2);
}

static void k_IsRectEmpty(rc_cpu *c) { RC_RETURN(c, empty(RC_ARG(c, 0)) ? 1 : 0, 1); }

static void k_EqualRect(rc_cpu *c)
{
    uint32_t a = RC_ARG(c, 0), b = RC_ARG(c, 1);
    int eq = 1;
    for (int i = 0; i < 4; i++) {
        eq &= rl(a, i) == rl(b, i);
    }
    RC_RETURN(c, (uint32_t)eq, 2);
}

static void k_InflateRect(rc_cpu *c)
{
    uint32_t r = RC_ARG(c, 0);
    int32_t dx = (int32_t)RC_ARG(c, 1), dy = (int32_t)RC_ARG(c, 2);
    rs(r, rl(r, 0) - dx, rl(r, 1) - dy, rl(r, 2) + dx, rl(r, 3) + dy);
    RC_RETURN(c, 1, 3);
}

static void k_PtInRect(rc_cpu *c)
{
    uint32_t r = RC_ARG(c, 0);
    int32_t x = (int32_t)RC_ARG(c, 1), y = (int32_t)RC_ARG(c, 2); /* POINT by value */
    RC_RETURN(c, x >= rl(r, 0) && x < rl(r, 2) && y >= rl(r, 1) && y < rl(r, 3) ? 1 : 0, 3);
}

static void k_IntersectRect(rc_cpu *c)
{
    uint32_t d = RC_ARG(c, 0), a = RC_ARG(c, 1), b = RC_ARG(c, 2);
    int32_t l = mx(rl(a, 0), rl(b, 0)), t = mx(rl(a, 1), rl(b, 1));
    int32_t r = mn(rl(a, 2), rl(b, 2)), bt = mn(rl(a, 3), rl(b, 3));
    if (empty(a) || empty(b) || r <= l || bt <= t) {
        rs(d, 0, 0, 0, 0);
        RC_RETURN(c, 0, 3);
    }
    rs(d, l, t, r, bt);
    RC_RETURN(c, 1, 3);
}

static void k_UnionRect(rc_cpu *c)
{
    uint32_t d = RC_ARG(c, 0), a = RC_ARG(c, 1), b = RC_ARG(c, 2);
    if (empty(a) && empty(b)) {
        rs(d, 0, 0, 0, 0);
        RC_RETURN(c, 0, 3);
    }
    if (empty(a)) {
        rs(d, rl(b, 0), rl(b, 1), rl(b, 2), rl(b, 3));
    } else if (empty(b)) {
        rs(d, rl(a, 0), rl(a, 1), rl(a, 2), rl(a, 3));
    } else {
        rs(d, mn(rl(a, 0), rl(b, 0)), mn(rl(a, 1), rl(b, 1)), mx(rl(a, 2), rl(b, 2)), mx(rl(a, 3), rl(b, 3)));
    }
    RC_RETURN(c, 1, 3);
}

/* Window frames: what Windows XP's classic theme adds around a client area. */
#define WS_CAPTION 0x00c00000u
#define WS_THICKFRAME 0x00040000u
#define WS_BORDER 0x00800000u

static void adjust(uint32_t r, uint32_t style, uint32_t menu)
{
    int32_t frame = (style & WS_THICKFRAME) ? 4 : (style & WS_BORDER) ? 3 : 0;
    int32_t caption = (style & WS_CAPTION) == WS_CAPTION ? 19 : 0;
    int32_t m = menu ? 20 : 0;
    rc_st32(r, (uint32_t)((int32_t)rc_ld32(r) - frame));
    rc_st32(r + 4, (uint32_t)((int32_t)rc_ld32(r + 4) - frame - caption - m));
    rc_st32(r + 8, (uint32_t)((int32_t)rc_ld32(r + 8) + frame));
    rc_st32(r + 12, (uint32_t)((int32_t)rc_ld32(r + 12) + frame));
}

static void k_AdjustWindowRect(rc_cpu *c)
{
    adjust(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2));
    RC_RETURN(c, 1, 3);
}

static void k_AdjustWindowRectEx(rc_cpu *c)
{
    adjust(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2));
    RC_RETURN(c, 1, 4);
}

const rc_import_def rc_user32_setup[] = {
    {U32, "RegisterClassA", k_RegisterClassA},
    {U32, "RegisterClassExA", k_RegisterClassExA},
    {U32, "GetClassInfoA", k_GetClassInfoA},
    {U32, "GetClassInfoExA", k_GetClassInfoExA},
    {U32, "UnregisterClassA", k_UnregisterClassA},
    {U32, "LoadCursorA", k_LoadCursorA},
    {U32, "LoadCursorFromFileA", k_LoadCursorFromFileA},
    {U32, "wsprintfA", k_wsprintfA},
    {U32, "wvsprintfA", k_wvsprintfA},
    {U32, "OpenClipboard", k_OpenClipboard},
    {U32, "GetClipboardData", k_GetClipboardData},
    {U32, "CloseClipboard", k_CloseClipboard},
    {U32, "EmptyClipboard", k_EmptyClipboard},
    {U32, "SetClipboardData", k_SetClipboardData},
    {U32, "IsClipboardFormatAvailable", k_IsClipboardFormatAvailable},
    {U32, "RegisterClipboardFormatA", k_RegisterClipboardFormatA},
    {U32, "SetCursor", k_SetCursor},
    {U32, "ShowCursor", k_ShowCursor},
    {U32, "LoadIconA", k_LoadIconA},
    {U32, "LoadAcceleratorsA", k_LoadAcceleratorsA},
    {U32, "TranslateAcceleratorA", k_TranslateAcceleratorA},
    {U32, "CreateIconIndirect", k_CreateIconIndirect},
    {U32, "DestroyCursor", k_DestroyCursor},
    {U32, "DestroyIcon", k_DestroyCursor},
    {U32, "RegisterWindowMessageA", k_RegisterWindowMessageA},
    {U32, "LoadStringA", k_LoadStringA},
    {U32, "SetRect", k_SetRect},
    {U32, "OffsetRect", k_OffsetRect},
    {U32, "AdjustWindowRect", k_AdjustWindowRect},
    {U32, "AdjustWindowRectEx", k_AdjustWindowRectEx},
    {U32, "SetRectEmpty", k_SetRectEmpty},
    {U32, "CopyRect", k_CopyRect},
    {U32, "IsRectEmpty", k_IsRectEmpty},
    {U32, "EqualRect", k_EqualRect},
    {U32, "InflateRect", k_InflateRect},
    {U32, "PtInRect", k_PtInRect},
    {U32, "IntersectRect", k_IntersectRect},
    {U32, "UnionRect", k_UnionRect},
};
const uint32_t rc_user32_setup_count = sizeof rc_user32_setup / sizeof rc_user32_setup[0];
