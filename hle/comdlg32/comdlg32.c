/* SPDX-License-Identifier: MIT */
/*
 * COMDLG32: the open and save dialogs, and GetFileTitleA. The dialog is our own (a #32770 window
 * with user32's controls): a file name, the files of the folder that match the chosen type, the
 * folders, and the types from the program's filter. It browses the program's installation folder,
 * as the program sees it (the user directory over the game's), and returns guest paths. Hooks and
 * custom templates are not run: the dialog does what the program asks of it by its flags.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "comdlg32.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel32/k32.h"
#include "rc/hostfs.h"
#include "rc/log.h"
#include "rc/process.h"
#include "rc/vm.h"
#include "user32/user32.h"

#define CD "comdlg32.dll"

#define WM_SETTEXT 0x000cu
#define WM_GETTEXT 0x000du
#define WM_INITDIALOG 0x0110u
#define WM_COMMAND 0x0111u
#define LB_ADDSTRING 0x0180u
#define LB_RESETCONTENT 0x0184u
#define LB_SETCURSEL 0x0186u
#define LB_GETCURSEL 0x0188u
#define LB_GETTEXT 0x0189u
#define CB_ADDSTRING 0x0143u
#define CB_GETCURSEL 0x0147u
#define CB_SETCURSEL 0x014eu
#define LBN_SELCHANGE 1u
#define LBN_DBLCLK 2u
#define CBN_SELCHANGE 1u

#define OFN_READONLY 0x1u
#define OFN_OVERWRITEPROMPT 0x2u
#define OFN_HIDEREADONLY 0x4u
#define OFN_PATHMUSTEXIST 0x800u
#define OFN_FILEMUSTEXIST 0x1000u
#define OFN_CREATEPROMPT 0x2000u

/* control ids, Windows' own (dlgs.h) */
#define ID_PATH 0x440u   /* stc1 */
#define ID_FILES 0x460u  /* lst1 */
#define ID_DIRS 0x461u   /* lst2 */
#define ID_TYPES 0x470u  /* cmb1 */
#define ID_NAME 0x480u   /* edt1 */

#define NAMES 2048
#define FILTERS 16

/* the dialog that is open (they are modal, so there is one) */
static struct {
    bool save;
    uint32_t ofn, hwnd, flags;
    char rel[RC_PATH_MAX]; /* the folder shown, '/'-separated, below the installation folder */
    char pattern[256];     /* typed into the name field, over the chosen type's */
    int nfilters;
    char filter[FILTERS][256]; /* the types' patterns, ';'-separated */
} fd;

static char last_rel[RC_PATH_MAX]; /* the folder of the last dialog, for the next */
static uint32_t scratch;           /* a guest buffer for the controls' strings */
static uint32_t error;             /* CommDlgExtendedError */

static uint32_t guest_text(const char *s)
{
    if (!scratch) {
        scratch = rc_heap_alloc(1024, true);
    }
    rc_put_str(scratch, 1024, s);
    return scratch;
}

static uint32_t item(uint32_t id)
{
    for (uint32_t k = rc_wnd_first_child(fd.hwnd); k; k = rc_wnd_next_sibling(k)) {
        if (rc_wnd_id(k) == id) {
            return k;
        }
    }
    return 0;
}

static void send_text(rc_cpu *c, uint32_t id, uint32_t msg, const char *s)
{
    rc_wnd_send(c, item(id), msg, 0, guest_text(s));
}

static void get_text(rc_cpu *c, uint32_t id, uint32_t msg, uint32_t wp, char *out, size_t cap)
{
    guest_text("");
    rc_wnd_send(c, item(id), msg, msg == WM_GETTEXT ? 1024 : wp, scratch);
    snprintf(out, cap, "%s", rc_str(scratch));
}

static int cmp_nocase(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) {
            return x - y;
        }
    }
}

/* `*` and `?`, without case */
static bool match(const char *p, const char *s)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            for (const char *t = s;; t++) {
                if (match(p + 1, t)) {
                    return true;
                }
                if (!*t) {
                    return false;
                }
            }
        }
        if (!*s || (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s))) {
            return false;
        }
    }
    return !*s;
}

/* one of the ';'-separated patterns ("*.*" matches names without a dot as well) */
static bool match_any(const char *patterns, const char *name)
{
    for (const char *p = patterns; *p;) {
        const char *end = strchr(p, ';');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char one[256];
        snprintf(one, sizeof one, "%.*s", (int)n, p);
        const char *q = one;
        while (*q == ' ') {
            q++;
        }
        if (*q && (!strcmp(q, "*.*") || match(q, name))) {
            return true;
        }
        p += n + (end ? 1 : 0);
    }
    return false;
}

/* ---- the folder's contents ---- */

typedef struct names {
    char (*name)[260];
    bool *dir;
    int n;
} names;

static void add(void *ctx, const char *name, bool is_dir, uint64_t size)
{
    (void)size;
    names *k = ctx;
    if (!strcmp(name, ".") || !strcmp(name, "..") || k->n == NAMES) {
        return;
    }
    for (int i = 0; i < k->n; i++) {
        if (!cmp_nocase(k->name[i], name)) {
            return; /* in both the user directory and the game's */
        }
    }
    snprintf(k->name[k->n], 260, "%s", name);
    k->dir[k->n++] = is_dir;
}

static void list_folder(const char *rel, names *k)
{
    char host[RC_PATH_MAX];
    if (rc_host_resolve(rc_cfg->user_dir, rel, false, host)) {
        rc_host_list(host, add, k);
    }
    if (rc_host_resolve(rc_cfg->game_dir, rel, false, host)) {
        rc_host_list(host, add, k);
    }
}

/* the guest path of a folder or file below the installation folder */
static void guest_path(const char *rel, char *out, size_t cap)
{
    int w = snprintf(out, cap, "%s%s%s", rc_cfg->guest_dir, rel[0] ? "\\" : "", rel);
    if (w < 0 || (size_t)w >= cap) {
        snprintf(out, cap, "%s", rc_cfg->guest_dir); /* too long to show: the installation folder */
    }
    for (char *p = out; *p; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
}

static void refresh(rc_cpu *c)
{
    names k = {malloc(sizeof(char[260]) * NAMES), malloc(sizeof(bool) * NAMES), 0};
    if (!k.name || !k.dir) {
        free(k.name);
        free(k.dir);
        return;
    }
    list_folder(fd.rel, &k);
    /* names with their kind: sort by name, keep the kind alongside */
    for (int i = 1; i < k.n; i++) {
        for (int j = i; j > 0 && cmp_nocase(k.name[j - 1], k.name[j]) > 0; j--) {
            char t[260];
            memcpy(t, k.name[j], 260);
            memcpy(k.name[j], k.name[j - 1], 260);
            memcpy(k.name[j - 1], t, 260);
            bool d = k.dir[j];
            k.dir[j] = k.dir[j - 1];
            k.dir[j - 1] = d;
        }
    }
    int type = (int)rc_wnd_send(c, item(ID_TYPES), CB_GETCURSEL, 0, 0);
    const char *pat = fd.pattern[0] ? fd.pattern : (type >= 0 && type < fd.nfilters ? fd.filter[type] : "*.*");
    rc_wnd_send(c, item(ID_FILES), LB_RESETCONTENT, 0, 0);
    rc_wnd_send(c, item(ID_DIRS), LB_RESETCONTENT, 0, 0);
    if (fd.rel[0]) {
        send_text(c, ID_DIRS, LB_ADDSTRING, "[..]");
    }
    for (int i = 0; i < k.n; i++) {
        if (k.dir[i]) {
            char b[264];
            snprintf(b, sizeof b, "[%s]", k.name[i]);
            send_text(c, ID_DIRS, LB_ADDSTRING, b);
        } else if (match_any(pat, k.name[i])) {
            send_text(c, ID_FILES, LB_ADDSTRING, k.name[i]);
        }
    }
    char shown[RC_PATH_MAX];
    guest_path(fd.rel, shown, sizeof shown);
    send_text(c, ID_PATH, WM_SETTEXT, shown);
    free(k.name);
    free(k.dir);
}

/* ".." and "." taken out of a '/'-separated path below the installation folder */
static void normalise(char *rel)
{
    char out[RC_PATH_MAX] = "";
    char buf[RC_PATH_MAX];
    snprintf(buf, sizeof buf, "%s", rel);
    for (char *p = buf, *next; p; p = next) {
        next = strchr(p, '/');
        if (next) {
            *next++ = 0;
        }
        if (!*p) {
            continue;
        }
        if (!strcmp(p, ".")) {
            continue;
        }
        if (!strcmp(p, "..")) {
            char *s = strrchr(out, '/');
            if (s) {
                *s = 0;
            } else {
                out[0] = 0;
            }
            continue;
        }
        size_t n = strlen(out);
        int w = snprintf(out + n, sizeof out - n, "%s%s", n ? "/" : "", p);
        if (w < 0 || (size_t)w >= sizeof out - n) {
            break; /* too long: what fits */
        }
    }
    snprintf(rel, RC_PATH_MAX, "%s", out);
}

static void enter(rc_cpu *c, const char *rel)
{
    snprintf(fd.rel, sizeof fd.rel, "%s", rel);
    normalise(fd.rel);
    fd.pattern[0] = 0;
    send_text(c, ID_NAME, WM_SETTEXT, "");
    refresh(c);
}

static bool exists(const char *rel, bool *is_dir)
{
    char host[RC_PATH_MAX];
    uint64_t size;
    return rc_k32_find_existing(rel, host, is_dir, &size);
}

/* The name field's text: a pattern, a folder to enter, or the file (true: the dialog ends). */
static bool accept(rc_cpu *c)
{
    char text[RC_PATH_MAX];
    get_text(c, ID_NAME, WM_GETTEXT, 0, text, sizeof text);
    char *t = text;
    while (*t == ' ') {
        t++;
    }
    for (size_t n = strlen(t); n && t[n - 1] == ' '; n--) {
        t[n - 1] = 0;
    }
    if (!*t) {
        return false;
    }
    if (strpbrk(t, "*?")) {
        snprintf(fd.pattern, sizeof fd.pattern, "%.255s", t);
        refresh(c);
        return false;
    }
    char rel[RC_PATH_MAX];
    if ((t[0] && t[1] == ':') || t[0] == '\\') {
        if (!rc_k32_guest_rel(t, rel, sizeof rel)) {
            rc_message_box(c, fd.hwnd, "The path is outside the program's folder.", fd.save ? "Save As" : "Open", 0x30);
            return false;
        }
    } else {
        int w = snprintf(rel, sizeof rel, "%s%s%s", fd.rel, fd.rel[0] ? "/" : "", t);
        if (w < 0 || (size_t)w >= sizeof rel) {
            return false;
        }
        for (char *p = rel; *p; p++) {
            if (*p == '\\') {
                *p = '/';
            }
        }
    }
    normalise(rel);
    bool is_dir = false;
    bool there = exists(rel, &is_dir);
    if (there && is_dir) {
        enter(c, rel);
        return false;
    }
    const char *name = strrchr(rel, '/') ? strrchr(rel, '/') + 1 : rel;
    uint32_t defext = rc_ld32(fd.ofn + 60);
    if (!there && !strchr(name, '.') && defext && rc_ld8(defext)) {
        size_t n = strlen(rel);
        snprintf(rel + n, sizeof rel - n, ".%s", rc_str(defext));
        name = strrchr(rel, '/') ? strrchr(rel, '/') + 1 : rel;
        there = exists(rel, &is_dir);
    }
    char full[RC_PATH_MAX], msg[RC_PATH_MAX + 64];
    guest_path(rel, full, sizeof full);
    char folder[RC_PATH_MAX];
    snprintf(folder, sizeof folder, "%.*s", (int)(name - rel > 0 ? name - rel - 1 : 0), rel);
    bool dir_there = !folder[0] || (exists(folder, &is_dir) && is_dir);
    if (!dir_there && (fd.save || (fd.flags & (OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST)))) {
        snprintf(msg, sizeof msg, "%s\nThe folder does not exist.", full);
        rc_message_box(c, fd.hwnd, msg, fd.save ? "Save As" : "Open", 0x30);
        return false;
    }
    if (!fd.save && !there && (fd.flags & OFN_FILEMUSTEXIST)) {
        snprintf(msg, sizeof msg, "%s\nFile not found.\nCheck the file name.", full);
        rc_message_box(c, fd.hwnd, msg, "Open", 0x30);
        return false;
    }
    if (!fd.save && !there && (fd.flags & OFN_CREATEPROMPT)) {
        snprintf(msg, sizeof msg, "%s\nThis file does not exist.\nCreate it?", full);
        if (rc_message_box(c, fd.hwnd, msg, "Open", 0x34) != 6) {
            return false;
        }
    }
    if (fd.save && there && (fd.flags & OFN_OVERWRITEPROMPT)) {
        snprintf(msg, sizeof msg, "%s already exists.\nDo you want to replace it?", full);
        if (rc_message_box(c, fd.hwnd, msg, "Save As", 0x34) != 6) {
            return false;
        }
    }
    /* the result into OPENFILENAMEA */
    uint32_t file = rc_ld32(fd.ofn + 28), cap = rc_ld32(fd.ofn + 32);
    size_t len = strlen(full);
    if (!file || len + 1 > cap) {
        error = 0x3003; /* FNERR_BUFFERTOOSMALL: the size needed in the first word */
        if (file && cap >= 2) {
            rc_st16(file, (uint16_t)(len + 1));
        }
        rc_dialog_end(c, fd.hwnd, 0);
        return true;
    }
    rc_put_str(file, cap, full);
    const char *fname = strrchr(full, '\\') + 1;
    const char *dot = strrchr(fname, '.');
    rc_st16(fd.ofn + 56, (uint16_t)(fname - full));
    rc_st16(fd.ofn + 58, (uint16_t)(dot ? dot + 1 - full : 0));
    uint32_t title = rc_ld32(fd.ofn + 36), tcap = rc_ld32(fd.ofn + 40);
    if (title && tcap) {
        rc_put_str(title, tcap, fname);
    }
    int type = (int)rc_wnd_send(c, item(ID_TYPES), CB_GETCURSEL, 0, 0);
    if (type >= 0) {
        rc_st32(fd.ofn + 24, (uint32_t)type + 1);
    }
    snprintf(last_rel, sizeof last_rel, "%s", fd.rel);
    rc_dialog_end(c, fd.hwnd, 1);
    return true;
}

/* the dialog procedure */
static void k_FileDlgProc(rc_cpu *c)
{
    uint32_t hwnd = RC_ARG(c, 0), msg = RC_ARG(c, 1), wp = RC_ARG(c, 2);
    if (hwnd != fd.hwnd || msg != WM_COMMAND) {
        RC_RETURN(c, msg == WM_INITDIALOG, 4);
    }
    uint32_t id = wp & 0xffff, code = wp >> 16;
    char text[300];
    switch (id) {
    case 1: /* IDOK */
        accept(c);
        break;
    case 2: /* IDCANCEL */
        rc_dialog_end(c, hwnd, 0);
        break;
    case ID_FILES:
        if (code == LBN_SELCHANGE || code == LBN_DBLCLK) {
            int32_t i = (int32_t)rc_wnd_send(c, item(ID_FILES), LB_GETCURSEL, 0, 0);
            if (i >= 0) {
                get_text(c, ID_FILES, LB_GETTEXT, (uint32_t)i, text, sizeof text);
                send_text(c, ID_NAME, WM_SETTEXT, text);
                if (code == LBN_DBLCLK) {
                    accept(c);
                }
            }
        }
        break;
    case ID_DIRS:
        if (code == LBN_DBLCLK) {
            int32_t i = (int32_t)rc_wnd_send(c, item(ID_DIRS), LB_GETCURSEL, 0, 0);
            if (i >= 0) {
                get_text(c, ID_DIRS, LB_GETTEXT, (uint32_t)i, text, sizeof text);
                size_t n = strlen(text);
                if (n > 2) {
                    text[n - 1] = 0; /* "[name]" */
                    char rel[RC_PATH_MAX];
                    int w = snprintf(rel, sizeof rel, "%s%s%s", fd.rel, fd.rel[0] ? "/" : "", text + 1);
                    if (w > 0 && (size_t)w < sizeof rel) {
                        enter(c, rel);
                    }
                }
            }
        }
        break;
    case ID_TYPES:
        if (code == CBN_SELCHANGE) {
            fd.pattern[0] = 0;
            refresh(c);
        }
        break;
    default:
        break;
    }
    RC_RETURN(c, 1, 4);
}

static const rc_import_def proc_defs[] = {
    {"COMDLG32_PROC", "FileDlgProc", k_FileDlgProc},
};

/* The folder to start in: the program's initial folder, the folder of the name it passes, or the
 * last dialog's. */
static void start_folder(uint32_t ofn, char *name, size_t cap)
{
    uint32_t init = rc_ld32(ofn + 44), file = rc_ld32(ofn + 28);
    char rel[RC_PATH_MAX];
    snprintf(fd.rel, sizeof fd.rel, "%s", last_rel);
    name[0] = 0;
    if (file && rc_ld8(file)) {
        const char *f = rc_str(file);
        const char *sep = strrchr(f, '\\');
        if (sep) {
            char dir[RC_PATH_MAX];
            snprintf(dir, sizeof dir, "%.*s", (int)(sep - f), f);
            if (rc_k32_guest_rel(dir, rel, sizeof rel)) {
                snprintf(fd.rel, sizeof fd.rel, "%s", rel);
                init = 0;
            }
            snprintf(name, cap, "%s", sep + 1);
        } else {
            snprintf(name, cap, "%s", f);
        }
    }
    if (init && rc_ld8(init) && rc_k32_guest_rel(rc_str(init), rel, sizeof rel)) {
        snprintf(fd.rel, sizeof fd.rel, "%s", rel);
    }
    bool is_dir = false;
    if (fd.rel[0] && !(exists(fd.rel, &is_dir) && is_dir)) {
        fd.rel[0] = 0;
    }
}

static uint32_t file_dialog(rc_cpu *c, uint32_t ofn, bool save)
{
    error = 0;
    if (!rc_desktop_active()) {
        rc_log("comdlg32: a file dialog without a desktop is cancelled");
        return 0;
    }
    memset(&fd, 0, sizeof fd);
    fd.save = save;
    fd.ofn = ofn;
    fd.flags = rc_ld32(ofn + 52);
    uint32_t owner = rc_ld32(ofn + 4), filter = rc_ld32(ofn + 12), title = rc_ld32(ofn + 48);
    char name[RC_PATH_MAX];
    start_folder(ofn, name, sizeof name);
    if (strpbrk(name, "*?")) {
        snprintf(fd.pattern, sizeof fd.pattern, "%.255s", name);
    }
    const char *caption = title && rc_ld8(title) ? rc_str(title) : save ? "Save As" : "Open";
    uint32_t proc = rc_import_address("COMDLG32_PROC", "FileDlgProc");
    uint32_t hwnd = rc_dialog_create(c, owner, caption, 430, 290, proc);
    if (!hwnd) {
        return 0;
    }
    fd.hwnd = hwnd;
    rc_dialog_item(c, hwnd, "Static", "File &name:", 0, 10, 8, 190, 16, 0xffff);
    uint32_t edit = rc_dialog_item(c, hwnd, "Edit", "", WS_BORDER | WS_TABSTOP | WS_GROUP | 0x80 /* ES_AUTOHSCROLL */, 10,
                                   26, 190, 22, ID_NAME);
    rc_dialog_item(c, hwnd, "ListBox", "", WS_BORDER | WS_VSCROLL | WS_TABSTOP | 0x1 /* LBS_NOTIFY */, 10, 52, 190, 160,
                   ID_FILES);
    rc_dialog_item(c, hwnd, "Static", "&Folders:", WS_GROUP, 215, 8, 205, 16, 0xffff);
    rc_dialog_item(c, hwnd, "Static", "", 0x80 /* SS_NOPREFIX */ | 0xc /* SS_LEFTNOWORDWRAP */, 215, 28, 205, 16,
                   ID_PATH);
    rc_dialog_item(c, hwnd, "ListBox", "", WS_BORDER | WS_VSCROLL | WS_TABSTOP | 0x1, 215, 52, 205, 160, ID_DIRS);
    rc_dialog_item(c, hwnd, "Static", save ? "Save as &type:" : "Files of &type:", WS_GROUP, 10, 222, 190, 16, 0xffff);
    uint32_t types = rc_dialog_item(c, hwnd, "ComboBox", "", WS_VSCROLL | WS_TABSTOP | 0x3 /* CBS_DROPDOWNLIST */, 10,
                                    240, 190, 150, ID_TYPES);
    rc_dialog_item(c, hwnd, "Button", save ? "&Save" : "&Open", WS_TABSTOP | WS_GROUP | 0x1 /* BS_DEFPUSHBUTTON */, 345,
                   222, 75, 23, 1);
    rc_dialog_item(c, hwnd, "Button", "Cancel", WS_TABSTOP, 345, 252, 75, 23, 2);
    /* the types: pairs of description and patterns, ending with an empty one */
    for (uint32_t p = filter; p && rc_ld8(p) && fd.nfilters < FILTERS;) {
        const char *desc = rc_str(p);
        uint32_t pats = p + (uint32_t)strlen(desc) + 1;
        snprintf(fd.filter[fd.nfilters++], 256, "%s", rc_str(pats));
        send_text(c, ID_TYPES, CB_ADDSTRING, desc);
        p = pats + (uint32_t)strlen(rc_str(pats)) + 1;
    }
    if (!fd.nfilters) {
        snprintf(fd.filter[fd.nfilters++], 256, "*.*");
        send_text(c, ID_TYPES, CB_ADDSTRING, "All files (*.*)");
    }
    uint32_t index = rc_ld32(ofn + 24);
    rc_wnd_send(c, types, CB_SETCURSEL, index >= 1 && index <= (uint32_t)fd.nfilters ? index - 1 : 0, 0);
    refresh(c);
    if (!strpbrk(name, "*?")) {
        send_text(c, ID_NAME, WM_SETTEXT, name);
    }
    uint32_t r = rc_dialog_run(c, hwnd, owner, edit);
    fd.hwnd = 0;
    return r;
}

/* GetOpenFileNameA(OPENFILENAMEA *) */
static void k_GetOpenFileNameA(rc_cpu *c) { RC_RETURN(c, file_dialog(c, RC_ARG(c, 0), false), 1); }

/* GetSaveFileNameA(OPENFILENAMEA *) */
static void k_GetSaveFileNameA(rc_cpu *c) { RC_RETURN(c, file_dialog(c, RC_ARG(c, 0), true), 1); }

/* GetFileTitleA(path, buffer, size): the name without its folder; 0, or the size needed. */
static void k_GetFileTitleA(rc_cpu *c)
{
    const char *path = rc_str(RC_ARG(c, 0));
    const char *t = path;
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/' || *p == ':') {
            t = p + 1;
        }
    }
    uint32_t need = (uint32_t)strlen(t) + 1, buf = RC_ARG(c, 1), cap = RC_ARG(c, 2) & 0xffff;
    if (!*t) {
        RC_RETURN(c, 0xffffffffu, 3);
    }
    if (!buf || cap < need) {
        RC_RETURN(c, need, 3);
    }
    rc_put_str(buf, cap, t);
    RC_RETURN(c, 0, 3);
}

static void k_CommDlgExtendedError(rc_cpu *c) { RC_RETURN(c, error, 0); }

static const rc_import_def defs[] = {
    {CD, "GetOpenFileNameA", k_GetOpenFileNameA},
    {CD, "GetSaveFileNameA", k_GetSaveFileNameA},
    {CD, "GetFileTitleA", k_GetFileTitleA},
    {CD, "CommDlgExtendedError", k_CommDlgExtendedError},
};

void rc_comdlg32_init(void)
{
    rc_register_imports(defs, sizeof defs / sizeof defs[0]);
    rc_register_imports(proc_defs, sizeof proc_defs / sizeof proc_defs[0]);
}
