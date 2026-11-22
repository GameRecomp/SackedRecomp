/* SPDX-License-Identifier: MIT */
/*
 * kernel32: files, directories and searches, on a virtual file system.
 *
 * Guest paths are made absolute against the current directory (the guest
 * installation directory) and must lie under the guest installation
 * directory. Underneath it, files are looked up without regard to case in
 * the user directory (what the game wrote) first and then in the game
 * directory (read only). Writing a file that exists only in the game
 * directory copies it to the user directory first.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "k32.h"
#include "rc/hostfs.h"
#include "rc/log.h"
#include "rc/vm.h"
#if defined(__EMSCRIPTEN__)
#include "rc/web.h"
/* The user directory lives in the browser's storage: save it soon. */
#define SAVED() rc_web_saved()
#else
#define SAVED() ((void)0)
#endif

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_ARCHIVE 0x20u

/* ---- paths ---- */

static int ieq_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') {
            x = (char)(x + 32);
        }
        if (y >= 'A' && y <= 'Z') {
            y = (char)(y + 32);
        }
        if (x != y) {
            return 0;
        }
    }
    return 1;
}

/* Split an absolute guest path into components, resolving "." and "..". */
static int split(const char *path, char comps[][256], int max)
{
    int n = 0;
    const char *p = path;
    while (*p) {
        while (*p == '\\' || *p == '/') {
            p++;
        }
        const char *e = p;
        while (*e && *e != '\\' && *e != '/') {
            e++;
        }
        size_t len = (size_t)(e - p);
        if (len == 0) {
            break;
        }
        if (len == 1 && p[0] == '.') {
            /* nothing */
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (n > 1) {
                n--;
            }
        } else if (n < max && len < 256) {
            memcpy(comps[n], p, len);
            comps[n][len] = 0;
            n++;
        }
        p = e;
    }
    return n;
}

/* The current directory: a guest path, the installation directory until the program changes it. */
static char cwd[RC_PATH_MAX];

static const char *current_dir(void) { return cwd[0] ? cwd : rc_cfg->guest_dir; }

/* A guest path made absolute (relative ones from the current directory), "." and ".." resolved,
 * '\\'-separated ("C:\\" for a drive's root). */
static void absolute(const char *in, char *out, size_t cap)
{
    char abs[RC_PATH_MAX];
    const char *base = current_dir();
    if (in[0] && in[1] == ':') {
        snprintf(abs, sizeof abs, "%s", in);
    } else if (in[0] == '\\' || in[0] == '/') {
        snprintf(abs, sizeof abs, "%c:%s", base[0], in);
    } else {
        int w = snprintf(abs, sizeof abs, "%s\\%s", base, in);
        if (w < 0 || (size_t)w >= sizeof abs) {
            abs[0] = 0;
        }
    }
    static char comps[64][256];
    int k = split(abs, comps, 64);
    size_t used = 0;
    out[0] = 0;
    for (int i = 0; i < k && used < cap; i++) {
        int w = snprintf(out + used, cap - used, "%s%s", i ? "\\" : "", comps[i]);
        used += w > 0 ? (size_t)w : 0;
        if (i == 0 && k == 1 && used < cap) {
            used += (size_t)snprintf(out + used, cap - used, "\\");
        }
    }
}

/* SetCurrentDirectoryA's work: an existing directory inside the installation directory. */
bool rc_k32_set_current_dir(const char *path)
{
    char abs[RC_PATH_MAX], rel[RC_PATH_MAX], host[RC_PATH_MAX];
    absolute(path, abs, sizeof abs);
    bool is_dir = false;
    if (!rc_k32_guest_rel(abs, rel, sizeof rel) || !rc_k32_find_existing(rel, host, &is_dir, NULL) || !is_dir) {
        return false;
    }
    size_t n = strlen(abs);
    if (n > 3 && abs[n - 1] == '\\') {
        abs[n - 1] = 0;
    }
    snprintf(cwd, sizeof cwd, "%s", abs);
    return true;
}

/* The path below the guest installation directory, '/'-separated, or
 * false if the guest path lies elsewhere. */
bool rc_k32_guest_rel(const char *in, char *rel, size_t cap)
{
    if (!in) {
        return false;
    }
    char abs[RC_PATH_MAX];
    absolute(in, abs, sizeof abs);
    const char *root = rc_cfg->guest_dir;
    static char a[64][256], r[16][256];
    int na = split(abs, a, 64), nr = split(root, r, 16);
    if (na < nr) {
        return false;
    }
    for (int i = 0; i < nr; i++) {
        if (strlen(a[i]) != strlen(r[i]) || !ieq_n(a[i], r[i], strlen(r[i]))) {
            return false;
        }
    }
    rel[0] = 0;
    size_t used = 0;
    for (int i = nr; i < na; i++) {
        int w = snprintf(rel + used, cap - used, "%s%s", used ? "/" : "", a[i]);
        if (w < 0 || (size_t)w >= cap - used) {
            return false;
        }
        used += (size_t)w;
    }
    return true;
}

/* Where an existing file is: the user directory first. */
bool rc_k32_find_existing(const char *rel, char *host, bool *is_dir, uint64_t *size)
{
    if (rc_host_resolve(rc_cfg->user_dir, rel, false, host) && rc_host_exists(host, is_dir, size)) {
        return true;
    }
    return rc_host_resolve(rc_cfg->game_dir, rel, false, host) && rc_host_exists(host, is_dir, size);
}

/* ---- handles ---- */

enum kind { K_FREE, K_STD, K_FILE, K_FIND, K_MAPPING };

typedef struct entry {
    char name[260];
    bool is_dir;
    uint64_t size;
} entry;

typedef struct handle {
    enum kind kind;
    FILE *f;
    int std;
    entry *list;
    uint32_t count, next;
    uint32_t file; /* a mapping: the file handle it maps */
    /* A mapping of no file (page-file backed, shared by name): its memory and size. */
    uint32_t shared, shared_size;
    char map_name[64];
    /* A file's position when known without asking the C library (games ask
     * for it, and set it to where it is, many times a frame; each would be
     * a system call): set by SetFilePointer, moved by ReadFile, forgotten
     * by WriteFile. */
    long pos;
    int pos_known;
} handle;

#define HANDLES 256u
#define HANDLE_BASE 0x1000u
static handle handles[HANDLES];

static uint32_t new_handle(enum kind k)
{
    for (uint32_t i = 1; i < HANDLES; i++) {
        if (handles[i].kind == K_FREE) {
            memset(&handles[i], 0, sizeof handles[i]);
            handles[i].kind = k;
            return HANDLE_BASE + 4 * i;
        }
    }
    rc_fatal("out of file handles");
}

static handle *get(uint32_t h, enum kind k)
{
    if (h < HANDLE_BASE || (h - HANDLE_BASE) % 4) {
        return NULL;
    }
    uint32_t i = (h - HANDLE_BASE) / 4;
    return i < HANDLES && handles[i].kind == k ? &handles[i] : NULL;
}

static uint32_t std_handles[3];

static void k_GetStdHandle(rc_cpu *c)
{
    uint32_t which = RC_ARG(c, 0); /* -10, -11, -12 */
    uint32_t i = 0xfffffff6u - which;
    if (i > 2) {
        RC_RETURN(c, INVALID_HANDLE, 1);
    }
    if (!std_handles[i]) {
        std_handles[i] = new_handle(K_STD);
        get(std_handles[i], K_STD)->std = (int)i;
    }
    RC_RETURN(c, std_handles[i], 1);
}

static void k_SetStdHandle(rc_cpu *c) { RC_RETURN(c, 1, 2); }
static void k_SetHandleCount(rc_cpu *c) { RC_RETURN(c, RC_ARG(c, 0), 1); }

static void k_GetFileType(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0);
    if (get(h, K_STD)) {
        RC_RETURN(c, 2, 1); /* FILE_TYPE_CHAR */
    }
    RC_RETURN(c, get(h, K_FILE) ? 1 : 0, 1);
}

static bool copy_file(const char *from, const char *to)
{
    uint32_t n;
    uint8_t *b = rc_host_read(from, &n);
    if (!b) {
        return false;
    }
    FILE *f = fopen(to, "wb");
    bool ok = f && fwrite(b, 1, n, f) == n;
    if (f) {
        ok = fclose(f) == 0 && ok;
    }
    free(b);
    SAVED();
    return ok;
}

static void k_CreateFileA(rc_cpu *c)
{
    const char *name = rc_str(RC_ARG(c, 0));
    uint32_t access = RC_ARG(c, 1), disp = RC_ARG(c, 4);
    char rel[RC_PATH_MAX], host[RC_PATH_MAX];
    if (!rc_k32_guest_rel(name, rel, sizeof rel)) {
        rc_last_error = ERROR_PATH_NOT_FOUND;
        RC_RETURN(c, INVALID_HANDLE, 7);
    }
    bool is_dir = false;
    bool exists = rc_k32_find_existing(rel, host, &is_dir, NULL);
    if (exists && is_dir) {
        rc_last_error = ERROR_ACCESS_DENIED;
        RC_RETURN(c, INVALID_HANDLE, 7);
    }
    if (!exists && (disp == OPEN_EXISTING || disp == TRUNCATE_EXISTING)) {
        if (rc_trace("files")) {
            rc_log("[file] open \"%s\": not found", name);
        }
        rc_last_error = ERROR_FILE_NOT_FOUND;
        RC_RETURN(c, INVALID_HANDLE, 7);
    }
    if (exists && disp == CREATE_NEW) {
        rc_last_error = ERROR_FILE_EXISTS;
        RC_RETURN(c, INVALID_HANDLE, 7);
    }
    const char *mode = "rb";
    if (access & GENERIC_WRITE) {
        char target[RC_PATH_MAX];
        if (!rc_host_resolve(rc_cfg->user_dir, rel, true, target)) {
            rc_last_error = ERROR_PATH_NOT_FOUND;
            RC_RETURN(c, INVALID_HANDLE, 7);
        }
        bool truncate = disp == CREATE_ALWAYS || disp == TRUNCATE_EXISTING || !exists;
        if (exists && !truncate && strcmp(host, target) != 0 && !copy_file(host, target)) {
            rc_last_error = ERROR_ACCESS_DENIED;
            RC_RETURN(c, INVALID_HANDLE, 7);
        }
        snprintf(host, sizeof host, "%s", target);
        mode = truncate ? "w+b" : "r+b";
    }
    FILE *f = fopen(host, mode);
    if (rc_trace("files")) {
        rc_log("[file] open \"%s\" (%s) -> %s", name, mode, f ? host : "failed");
    }
    if (!f) {
        rc_last_error = ERROR_ACCESS_DENIED;
        RC_RETURN(c, INVALID_HANDLE, 7);
    }
    uint32_t h = new_handle(K_FILE);
    get(h, K_FILE)->f = f;
    rc_last_error = exists && (disp == OPEN_ALWAYS || disp == CREATE_ALWAYS) ? 183u : ERROR_SUCCESS;
    RC_RETURN(c, h, 7);
}

static void k_ReadFile(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0), buf = RC_ARG(c, 1), n = RC_ARG(c, 2), done = RC_ARG(c, 3);
    handle *x = get(h, K_FILE);
    if (!x) {
        if (done) {
            rc_st32(done, 0);
        }
        rc_last_error = ERROR_INVALID_HANDLE;
        RC_RETURN(c, get(h, K_STD) != NULL, 5);
    }
    size_t r = rc_host_fread(rc_ptr(buf), n, x->f);
    x->pos += (long)r;
    if (done) {
        rc_st32(done, (uint32_t)r);
    }
    RC_RETURN(c, 1, 5);
}

static void k_WriteFile(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0), buf = RC_ARG(c, 1), n = RC_ARG(c, 2), done = RC_ARG(c, 3);
    size_t w = 0;
    handle *x = get(h, K_FILE);
    handle *s = get(h, K_STD);
    if (x) {
        w = fwrite(rc_ptr(buf), 1, n, x->f);
        x->pos_known = 0;
        SAVED();
    } else if (s) {
        w = fwrite(rc_ptr(buf), 1, n, s->std == 2 ? stderr : stdout);
    } else {
        rc_last_error = ERROR_INVALID_HANDLE;
        RC_RETURN(c, 0, 5);
    }
    if (done) {
        rc_st32(done, (uint32_t)w);
    }
    RC_RETURN(c, w == n, 5);
}

static void k_SetFilePointer(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0), method = RC_ARG(c, 3), high = RC_ARG(c, 2);
    int32_t dist = (int32_t)RC_ARG(c, 1);
    handle *x = get(h, K_FILE);
    if (!x || method > 2) {
        rc_last_error = ERROR_INVALID_HANDLE;
        RC_RETURN(c, 0xffffffffu, 4);
    }
    if (high) {
        rc_st32(high, 0);
    }
    if (x->pos_known && ((method == 1 && dist == 0) || (method == 0 && dist == x->pos))) {
        RC_RETURN(c, (uint32_t)x->pos, 4);
    }
    int whence = method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END;
    if (fseek(x->f, dist, whence) != 0) {
        x->pos_known = 0;
        rc_last_error = 131; /* ERROR_NEGATIVE_SEEK */
        RC_RETURN(c, 0xffffffffu, 4);
    }
    x->pos = ftell(x->f);
    x->pos_known = x->pos >= 0;
    RC_RETURN(c, (uint32_t)x->pos, 4);
}

static void k_SetEndOfFile(rc_cpu *c)
{
    handle *x = get(RC_ARG(c, 0), K_FILE);
    if (!x) {
        RC_RETURN(c, 0, 1);
    }
    fflush(x->f);
    long pos = ftell(x->f);
#if defined(_WIN32)
    int ok = _chsize(_fileno(x->f), pos) == 0;
#else
    int ok = ftruncate(fileno(x->f), pos) == 0;
#endif
    SAVED();
    RC_RETURN(c, ok, 1);
}

static void k_FlushFileBuffers(rc_cpu *c)
{
    handle *x = get(RC_ARG(c, 0), K_FILE);
    RC_RETURN(c, x ? fflush(x->f) == 0 : 1, 1);
}

static void k_CloseHandle(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0);
    handle *x = get(h, K_FILE);
    if (x) {
        fclose(x->f);
        x->kind = K_FREE;
        SAVED();
    }
    handle *m = get(h, K_MAPPING);
    if (m) {
        m->kind = K_FREE;
    }
    RC_RETURN(c, 1, 1); /* events and other pseudo handles close too */
}

static void k_GetFileAttributesA(rc_cpu *c)
{
    char rel[RC_PATH_MAX], host[RC_PATH_MAX];
    bool is_dir = false;
    if (!rc_k32_guest_rel(rc_str(RC_ARG(c, 0)), rel, sizeof rel) || !rc_k32_find_existing(rel, host, &is_dir, NULL)) {
        rc_last_error = ERROR_FILE_NOT_FOUND;
        RC_RETURN(c, 0xffffffffu, 1);
    }
    RC_RETURN(c, is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE, 1);
}

/* For other modules: read a guest file's first bytes, by name or by an
 * open handle. Returns the number of bytes read, or -1. */
long rc_vfs_read_head(const char *guest_path, uint32_t h, uint8_t *buf, uint32_t n)
{
    if (guest_path) {
        char rel[RC_PATH_MAX], host[RC_PATH_MAX];
        if (!rc_k32_guest_rel(guest_path, rel, sizeof rel) || !rc_k32_find_existing(rel, host, NULL, NULL)) {
            return -1;
        }
        FILE *f = fopen(host, "rb");
        if (!f) {
            return -1;
        }
        long r = (long)rc_host_fread(buf, n, f);
        fclose(f);
        return r;
    }
    handle *x = get(h, K_FILE);
    if (!x) {
        return -1;
    }
    long pos = ftell(x->f);
    fseek(x->f, 0, SEEK_SET);
    long r = (long)rc_host_fread(buf, n, x->f);
    fseek(x->f, pos, SEEK_SET);
    return r;
}

/* ---- searches ---- */

static bool wild(const char *p, const char *s)
{
    for (;;) {
        if (*p == '*') {
            while (*p == '*') {
                p++;
            }
            if (!*p) {
                return true;
            }
            /* "*.*" also matches names without a dot. */
            if (p[0] == '.' && p[1] == '*' && !p[2]) {
                return true;
            }
            for (; *s; s++) {
                if (wild(p, s)) {
                    return true;
                }
            }
            return wild(p, s);
        }
        if (!*p) {
            return !*s;
        }
        if (!*s) {
            return false;
        }
        if (*p != '?' && !ieq_n(p, s, 1)) {
            return false;
        }
        p++;
        s++;
    }
}

typedef struct collect {
    const char *pattern;
    entry *list;
    uint32_t count, cap;
} collect;

static void add(void *ctx, const char *name, bool is_dir, uint64_t size)
{
    collect *k = ctx;
    if (!wild(k->pattern, name)) {
        return;
    }
    for (uint32_t i = 0; i < k->count; i++) {
        size_t n = strlen(name);
        if (strlen(k->list[i].name) == n && ieq_n(k->list[i].name, name, n)) {
            return; /* the user directory's copy came first */
        }
    }
    if (k->count == k->cap) {
        k->cap = k->cap ? 2 * k->cap : 64;
        k->list = realloc(k->list, k->cap * sizeof *k->list);
        if (!k->list) {
            rc_fatal("out of memory");
        }
    }
    entry *e = &k->list[k->count++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->is_dir = is_dir;
    e->size = size;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(((const entry *)a)->name, ((const entry *)b)->name);
}

static void fill_find_data(uint32_t p, const entry *e)
{
    memset(rc_ptr(p), 0, 320);
    rc_st32(p, e->is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE);
    /* A fixed time keeps runs reproducible: 2002-07-19 00:00 UTC. */
    for (uint32_t t = 4; t <= 20; t += 8) {
        rc_st32(p + t, 0x5a6b8000u);
        rc_st32(p + t + 4, 0x01c22ee1u);
    }
    rc_st32(p + 28, (uint32_t)(e->size >> 32));
    rc_st32(p + 32, (uint32_t)e->size);
    rc_put_str(p + 44, 260, e->name);
}

static void k_FindFirstFileA(rc_cpu *c)
{
    const char *pattern = rc_str(RC_ARG(c, 0));
    char rel[RC_PATH_MAX];
    if (!rc_k32_guest_rel(pattern, rel, sizeof rel)) {
        rc_last_error = ERROR_PATH_NOT_FOUND;
        RC_RETURN(c, INVALID_HANDLE, 2);
    }
    char *slash = strrchr(rel, '/');
    char dir[RC_PATH_MAX], pat[RC_PATH_MAX];
    if (slash) {
        *slash = 0;
        snprintf(dir, sizeof dir, "%s", rel);
        snprintf(pat, sizeof pat, "%s", slash + 1);
    } else {
        dir[0] = 0;
        snprintf(pat, sizeof pat, "%s", rel);
    }
    collect k = {pat, NULL, 0, 0};
    char host[RC_PATH_MAX];
    if (rc_host_resolve(rc_cfg->user_dir, dir, false, host)) {
        rc_host_list(host, add, &k);
    }
    if (rc_host_resolve(rc_cfg->game_dir, dir, false, host)) {
        rc_host_list(host, add, &k);
    }
    if (k.count == 0) {
        free(k.list);
        rc_last_error = ERROR_FILE_NOT_FOUND;
        RC_RETURN(c, INVALID_HANDLE, 2);
    }
    /* Host directory order is arbitrary; sorted is reproducible. */
    qsort(k.list, k.count, sizeof *k.list, by_name);
    uint32_t h = new_handle(K_FIND);
    handle *x = get(h, K_FIND);
    x->list = k.list;
    x->count = k.count;
    x->next = 1;
    fill_find_data(RC_ARG(c, 1), &k.list[0]);
    RC_RETURN(c, h, 2);
}

static void k_FindNextFileA(rc_cpu *c)
{
    handle *x = get(RC_ARG(c, 0), K_FIND);
    if (!x || x->next >= x->count) {
        rc_last_error = ERROR_NO_MORE_FILES;
        RC_RETURN(c, 0, 2);
    }
    fill_find_data(RC_ARG(c, 1), &x->list[x->next++]);
    RC_RETURN(c, 1, 2);
}

static void k_FindClose(rc_cpu *c)
{
    handle *x = get(RC_ARG(c, 0), K_FIND);
    if (x) {
        free(x->list);
        x->kind = K_FREE;
    }
    RC_RETURN(c, x != NULL, 1);
}

static void k_CopyFileA(rc_cpu *c)
{
    char rs[RC_PATH_MAX], rd[RC_PATH_MAX], hs[RC_PATH_MAX], hd[RC_PATH_MAX];
    if (!rc_k32_guest_rel(rc_str(RC_ARG(c, 0)), rs, sizeof rs) || !rc_k32_guest_rel(rc_str(RC_ARG(c, 1)), rd, sizeof rd) ||
        !rc_k32_find_existing(rs, hs, NULL, NULL)) {
        rc_last_error = ERROR_FILE_NOT_FOUND;
        RC_RETURN(c, 0, 3);
    }
    if (RC_ARG(c, 2) && rc_k32_find_existing(rd, hd, NULL, NULL)) {
        rc_last_error = ERROR_FILE_EXISTS;
        RC_RETURN(c, 0, 3);
    }
    if (!rc_host_resolve(rc_cfg->user_dir, rd, true, hd) || !copy_file(hs, hd)) {
        rc_last_error = ERROR_ACCESS_DENIED;
        RC_RETURN(c, 0, 3);
    }
    RC_RETURN(c, 1, 3);
}

static void k_GetDriveTypeA(rc_cpu *c)
{
    const char *root = rc_str(RC_ARG(c, 0));
    char drive = root && root[0] ? root[0] : rc_cfg->guest_dir[0];
    if (drive >= 'a' && drive <= 'z') {
        drive = (char)(drive - 32);
    }
    RC_RETURN(c, drive == rc_cfg->guest_dir[0] ? 3u : 1u, 1); /* DRIVE_FIXED : DRIVE_NO_ROOT_DIR */
}

/* GetVolumeInformationA(root, name, name_size, serial, max_component, flags, fs, fs_size):
 * the installation's drive, a hard disk with NTFS. */
static void k_GetVolumeInformationA(rc_cpu *c)
{
    const char *root = rc_str(RC_ARG(c, 0));
    char drive = root && root[0] ? root[0] : rc_cfg->guest_dir[0];
    if (drive >= 'a' && drive <= 'z') {
        drive = (char)(drive - 32);
    }
    if (drive != rc_cfg->guest_dir[0]) {
        rc_last_error = 21; /* ERROR_NOT_READY */
        RC_RETURN(c, 0, 8);
    }
    if (RC_ARG(c, 1) && RC_ARG(c, 2)) {
        rc_put_str(RC_ARG(c, 1), RC_ARG(c, 2), "SYSTEM");
    }
    if (RC_ARG(c, 3)) {
        rc_st32(RC_ARG(c, 3), 0x1c0de001u);
    }
    if (RC_ARG(c, 4)) {
        rc_st32(RC_ARG(c, 4), 255);
    }
    if (RC_ARG(c, 5)) {
        rc_st32(RC_ARG(c, 5), 0x0007u); /* case preserving and sensitive, unicode names */
    }
    if (RC_ARG(c, 6) && RC_ARG(c, 7)) {
        rc_put_str(RC_ARG(c, 6), RC_ARG(c, 7), "NTFS");
    }
    RC_RETURN(c, 1, 8);
}

/* GetDiskFreeSpaceA(root, sectors per cluster, bytes per sector, free, total): 2 GiB free
 * of 4, the most the old API reports. */
static void k_GetDiskFreeSpaceA(rc_cpu *c)
{
    if (RC_ARG(c, 1)) {
        rc_st32(RC_ARG(c, 1), 8);
    }
    if (RC_ARG(c, 2)) {
        rc_st32(RC_ARG(c, 2), 512);
    }
    if (RC_ARG(c, 3)) {
        rc_st32(RC_ARG(c, 3), 0x80000u);
    }
    if (RC_ARG(c, 4)) {
        rc_st32(RC_ARG(c, 4), 0x100000u);
    }
    RC_RETURN(c, 1, 5);
}

/* GetFileInformationByHandle(file, BY_HANDLE_FILE_INFORMATION*) */
static void k_GetFileInformationByHandle(rc_cpu *c)
{
    handle *x = get(RC_ARG(c, 0), K_FILE);
    uint32_t p = RC_ARG(c, 1);
    if (!x) {
        rc_last_error = ERROR_INVALID_HANDLE;
        RC_RETURN(c, 0, 2);
    }
    long at = ftell(x->f);
    fseek(x->f, 0, SEEK_END);
    long n = ftell(x->f);
    fseek(x->f, at, SEEK_SET);
    memset(rc_ptr(p), 0, 52);
    rc_st32(p, 0x80); /* FILE_ATTRIBUTE_NORMAL */
    rc_st32(p + 28, 0x1c0de001u);
    rc_st32(p + 36, (uint32_t)n);
    rc_st32(p + 40, 1);
    rc_st32(p + 48, RC_ARG(c, 0));
    RC_RETURN(c, 1, 2);
}

/* No pipes: the CRT asks this of standard handles that are not consoles. */
static void k_PeekNamedPipe(rc_cpu *c)
{
    rc_last_error = ERROR_INVALID_HANDLE;
    RC_RETURN(c, 0, 6);
}

static void k_GetCurrentDirectoryA(rc_cpu *c)
{
    uint32_t n = RC_ARG(c, 0), buf = RC_ARG(c, 1);
    const char *dir = current_dir();
    uint32_t len = (uint32_t)strlen(dir);
    if (n < len + 1) {
        RC_RETURN(c, len + 1, 2);
    }
    RC_RETURN(c, rc_put_str(buf, n, dir), 2);
}

static void k_GetFullPathNameA(rc_cpu *c)
{
    const char *in = rc_str(RC_ARG(c, 0));
    uint32_t n = RC_ARG(c, 1), buf = RC_ARG(c, 2), part = RC_ARG(c, 3);
    char out[RC_PATH_MAX];
    absolute(in, out, sizeof out);
    uint32_t len = (uint32_t)strlen(out);
    if (n < len + 1) {
        RC_RETURN(c, len + 1, 4);
    }
    rc_put_str(buf, n, out);
    if (part) {
        const char *s = strrchr(out, '\\');
        rc_st32(part, s && s[1] ? buf + (uint32_t)(s + 1 - out) : 0);
    }
    RC_RETURN(c, len, 4);
}

static void k_GetFileSize(rc_cpu *c)
{
    handle *x = get(RC_ARG(c, 0), K_FILE);
    uint32_t high = RC_ARG(c, 1);
    if (!x) {
        rc_last_error = ERROR_INVALID_HANDLE;
        RC_RETURN(c, 0xffffffffu, 2);
    }
    long at = ftell(x->f);
    fseek(x->f, 0, SEEK_END);
    long n = ftell(x->f);
    fseek(x->f, at, SEEK_SET);
    if (high) {
        rc_st32(high, 0);
    }
    RC_RETURN(c, (uint32_t)n, 2);
}

/* Only what the game wrote can be deleted (the user directory's copy). */
static void k_DeleteFileA(rc_cpu *c)
{
    char rel[RC_PATH_MAX], host[RC_PATH_MAX];
    bool is_dir = false;
    if (!rc_k32_guest_rel(rc_str(RC_ARG(c, 0)), rel, sizeof rel) ||
        !rc_host_resolve(rc_cfg->user_dir, rel, false, host) || !rc_host_exists(host, &is_dir, NULL) || is_dir) {
        rc_last_error = ERROR_FILE_NOT_FOUND;
        RC_RETURN(c, 0, 1);
    }
    int ok = remove(host) == 0;
    if (ok) {
        SAVED();
    } else {
        rc_last_error = ERROR_ACCESS_DENIED;
    }
    RC_RETURN(c, ok ? 1 : 0, 1);
}

/* MoveFileA(from, to): within the user directory a rename; a file of the installation is
 * copied (the installation is read-only). */
static void k_MoveFileA(rc_cpu *c)
{
    char from_rel[RC_PATH_MAX], to_rel[RC_PATH_MAX], from[RC_PATH_MAX], to[RC_PATH_MAX];
    bool is_dir = false;
    if (!rc_k32_guest_rel(rc_str(RC_ARG(c, 0)), from_rel, sizeof from_rel) ||
        !rc_k32_guest_rel(rc_str(RC_ARG(c, 1)), to_rel, sizeof to_rel) ||
        !rc_k32_find_existing(from_rel, from, &is_dir, NULL) || is_dir) {
        rc_last_error = ERROR_FILE_NOT_FOUND;
        RC_RETURN(c, 0, 2);
    }
    if (rc_host_resolve(rc_cfg->user_dir, to_rel, false, to) && rc_host_exists(to, NULL, NULL)) {
        rc_last_error = ERROR_FILE_EXISTS;
        RC_RETURN(c, 0, 2);
    }
    if (!rc_host_resolve(rc_cfg->user_dir, to_rel, true, to)) {
        rc_last_error = ERROR_PATH_NOT_FOUND;
        RC_RETURN(c, 0, 2);
    }
    char mine[RC_PATH_MAX];
    bool in_user = rc_host_resolve(rc_cfg->user_dir, from_rel, false, mine) && strcmp(mine, from) == 0;
    int ok = in_user ? rename(from, to) == 0 : copy_file(from, to);
    if (ok) {
        SAVED();
    } else {
        rc_last_error = ERROR_ACCESS_DENIED;
    }
    RC_RETURN(c, ok ? 1 : 0, 2);
}

/* GetTempFileNameA(dir, prefix, unique, out): dir\PREFxxxx.TMP; with unique 0 a new name,
 * created empty. */
static void k_GetTempFileNameA(rc_cpu *c)
{
    static uint32_t counter = 0x1000;
    const char *dir = rc_str(RC_ARG(c, 0)), *prefix = rc_str(RC_ARG(c, 1));
    uint32_t unique = RC_ARG(c, 2), out = RC_ARG(c, 3);
    char name[RC_PATH_MAX];
    for (int tries = 0; tries < 0x10000; tries++) {
        uint32_t n = unique ? unique & 0xffff : counter++ & 0xffff;
        snprintf(name, sizeof name, "%s%s%.3s%04X.TMP", dir ? dir : "",
                 dir && *dir && dir[strlen(dir) - 1] != '\\' ? "\\" : "", prefix ? prefix : "", n);
        if (unique) {
            break;
        }
        char rel[RC_PATH_MAX], host[RC_PATH_MAX];
        if (!rc_k32_guest_rel(name, rel, sizeof rel)) {
            break;
        }
        if (rc_k32_find_existing(rel, host, NULL, NULL)) {
            continue;
        }
        if (rc_host_resolve(rc_cfg->user_dir, rel, true, host)) {
            FILE *f = fopen(host, "wb");
            if (f) {
                fclose(f);
            }
        }
        break;
    }
    rc_put_str(out, RC_PATH_MAX < 260 ? RC_PATH_MAX : 260, name);
    RC_RETURN(c, unique ? unique : 1, 4);
}

static void k_CreateDirectoryA(rc_cpu *c)
{
    char rel[RC_PATH_MAX], host[RC_PATH_MAX];
    bool is_dir = false;
    if (!rc_k32_guest_rel(rc_str(RC_ARG(c, 0)), rel, sizeof rel)) {
        rc_last_error = ERROR_PATH_NOT_FOUND;
        RC_RETURN(c, 0, 2);
    }
    if (rc_k32_find_existing(rel, host, &is_dir, NULL)) {
        rc_last_error = 183; /* ERROR_ALREADY_EXISTS */
        RC_RETURN(c, 0, 2);
    }
    if (!rc_host_resolve(rc_cfg->user_dir, rel, true, host) || !rc_host_mkdir(host)) {
        rc_last_error = ERROR_PATH_NOT_FOUND;
        RC_RETURN(c, 0, 2);
    }
    SAVED();
    RC_RETURN(c, 1, 2);
}

/* ---- file mappings: a view is a copy of the file in guest memory, written
 * back when it is unmapped if it may be written. A mapping of no file
 * (INVALID_HANDLE_VALUE: page-file backed memory) is one block that every
 * view and every handle of the same name share. ---- */

#define FILE_MAP_WRITE 2u
#define VIEWS 64
static struct view {
    uint32_t addr, size, offset, file;
    bool write;
    bool vm; /* placed where MapViewOfFileEx asked: a reservation of its own */
} views[VIEWS];

/* The mapping named `name` (of no file), if any. */
static handle *named_mapping(const char *name)
{
    for (uint32_t i = 1; name && *name && i < HANDLES; i++) {
        if (handles[i].kind == K_MAPPING && handles[i].shared && strcmp(handles[i].map_name, name) == 0) {
            return &handles[i];
        }
    }
    return NULL;
}

/* A new handle for the same memory as the named mapping `m`. */
static uint32_t share_mapping(const handle *m)
{
    handle copy = *m;
    uint32_t h = new_handle(K_MAPPING);
    *get(h, K_MAPPING) = copy;
    return h;
}

/* CreateFileMappingA(file, security, protect, size high, size low, name) */
static void k_CreateFileMappingA(rc_cpu *c)
{
    uint32_t fh = RC_ARG(c, 0), size = RC_ARG(c, 4), name = RC_ARG(c, 5);
    const char *n = name ? rc_str(name) : NULL;
    if (fh == 0xffffffffu) {
        handle *old = named_mapping(n);
        if (old) {
            uint32_t h = share_mapping(old);
            rc_last_error = 183; /* ERROR_ALREADY_EXISTS */
            RC_RETURN(c, h, 6);
        }
        uint32_t mem = size ? rc_heap_alloc(size, true) : 0;
        if (!mem) {
            rc_last_error = ERROR_NOT_ENOUGH_MEMORY;
            RC_RETURN(c, 0, 6);
        }
        uint32_t h = new_handle(K_MAPPING);
        handle *m = get(h, K_MAPPING);
        m->shared = mem;
        m->shared_size = size;
        if (n) {
            strncpy(m->map_name, n, sizeof m->map_name - 1);
        }
        rc_last_error = ERROR_SUCCESS;
        RC_RETURN(c, h, 6);
    }
    if (!get(fh, K_FILE)) {
        rc_last_error = ERROR_INVALID_HANDLE;
        RC_RETURN(c, 0, 6);
    }
    uint32_t h = new_handle(K_MAPPING);
    get(h, K_MAPPING)->file = fh;
    rc_last_error = ERROR_SUCCESS;
    RC_RETURN(c, h, 6);
}

/* OpenFileMappingA(access, inherit, name): a page-file mapping created under that name. */
static void k_OpenFileMappingA(rc_cpu *c)
{
    uint32_t name = RC_ARG(c, 2);
    handle *m = named_mapping(name ? rc_str(name) : NULL);
    if (!m) {
        rc_last_error = 2; /* ERROR_FILE_NOT_FOUND */
        RC_RETURN(c, 0, 3);
    }
    rc_last_error = ERROR_SUCCESS;
    RC_RETURN(c, share_mapping(m), 3);
}

/* A view of `n` bytes at `offset` (n = 0: to the end), at `base` if not 0. */
static uint32_t map_view(uint32_t mh, uint32_t access, uint32_t offset, uint32_t n, uint32_t base)
{
    handle *m = get(mh, K_MAPPING);
    if (m && m->shared) {
        if (offset > m->shared_size || base) {
            rc_last_error = ERROR_INVALID_PARAMETER;
            return 0;
        }
        return m->shared + offset;
    }
    handle *x = m ? get(m->file, K_FILE) : NULL;
    if (!x) {
        rc_last_error = ERROR_INVALID_HANDLE;
        return 0;
    }
    long at = ftell(x->f);
    fseek(x->f, 0, SEEK_END);
    long size = ftell(x->f);
    if (n == 0) {
        n = size > (long)offset ? (uint32_t)(size - (long)offset) : 0;
    }
    int slot = -1;
    for (int i = 0; i < VIEWS && slot < 0; i++) {
        if (!views[i].addr) {
            slot = i;
        }
    }
    uint32_t addr = 0;
    if (slot >= 0 && n) {
        addr = base ? rc_vm_alloc(base, n, true) : rc_heap_alloc(n, true);
    }
    if (!addr || (base && addr != base)) {
        fseek(x->f, at, SEEK_SET);
        rc_last_error = base ? 487u /* ERROR_INVALID_ADDRESS */ : ERROR_NOT_ENOUGH_MEMORY;
        return 0;
    }
    fseek(x->f, (long)offset, SEEK_SET);
    size_t got = rc_host_fread(rc_ptr(addr), n, x->f);
    (void)got;
    fseek(x->f, at, SEEK_SET);
    views[slot] = (struct view){addr, n, offset, m->file, (access & FILE_MAP_WRITE) != 0, base != 0};
    return addr;
}

/* MapViewOfFile(mapping, access, offset high, offset low, size) */
static void k_MapViewOfFile(rc_cpu *c)
{
    RC_RETURN(c, map_view(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 3), RC_ARG(c, 4), 0), 5);
}

/* MapViewOfFileEx(mapping, access, offset high, offset low, size, base) */
static void k_MapViewOfFileEx(rc_cpu *c)
{
    RC_RETURN(c, map_view(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 3), RC_ARG(c, 4), RC_ARG(c, 5)), 6);
}

static void k_UnmapViewOfFile(rc_cpu *c)
{
    uint32_t addr = RC_ARG(c, 0);
    for (int i = 0; i < VIEWS; i++) {
        if (views[i].addr == addr && addr) {
            handle *x = views[i].write ? get(views[i].file, K_FILE) : NULL;
            if (x) {
                long at = ftell(x->f);
                fseek(x->f, (long)views[i].offset, SEEK_SET);
                fwrite(rc_ptr(addr), 1, views[i].size, x->f);
                fseek(x->f, at, SEEK_SET);
                SAVED();
            }
            if (views[i].vm) {
                rc_vm_release(addr);
            } else {
                rc_heap_free(addr);
            }
            views[i].addr = 0;
            RC_RETURN(c, 1, 1);
        }
    }
    /* A view of a page-file mapping stays as long as the memory. */
    for (uint32_t i = 1; i < HANDLES; i++) {
        handle *m = &handles[i];
        if (m->kind == K_MAPPING && m->shared && addr >= m->shared && addr < m->shared + m->shared_size) {
            RC_RETURN(c, 1, 1);
        }
    }
    RC_RETURN(c, 0, 1);
}

const rc_import_def rc_k32_file[] = {
    {K32, "GetFileSize", k_GetFileSize},
    {K32, "DeleteFileA", k_DeleteFileA},
    {K32, "CreateDirectoryA", k_CreateDirectoryA},
    {K32, "CreateFileMappingA", k_CreateFileMappingA},
    {K32, "MapViewOfFile", k_MapViewOfFile},
    {K32, "MapViewOfFileEx", k_MapViewOfFileEx},
    {K32, "OpenFileMappingA", k_OpenFileMappingA},
    {K32, "UnmapViewOfFile", k_UnmapViewOfFile},
    {K32, "GetStdHandle", k_GetStdHandle},
    {K32, "SetStdHandle", k_SetStdHandle},
    {K32, "SetHandleCount", k_SetHandleCount},
    {K32, "GetFileType", k_GetFileType},
    {K32, "CreateFileA", k_CreateFileA},
    {K32, "ReadFile", k_ReadFile},
    {K32, "WriteFile", k_WriteFile},
    {K32, "SetFilePointer", k_SetFilePointer},
    {K32, "SetEndOfFile", k_SetEndOfFile},
    {K32, "FlushFileBuffers", k_FlushFileBuffers},
    {K32, "CloseHandle", k_CloseHandle},
    {K32, "GetFileAttributesA", k_GetFileAttributesA},
    {K32, "FindFirstFileA", k_FindFirstFileA},
    {K32, "FindNextFileA", k_FindNextFileA},
    {K32, "FindClose", k_FindClose},
    {K32, "CopyFileA", k_CopyFileA},
    {K32, "GetDriveTypeA", k_GetDriveTypeA},
    {K32, "GetVolumeInformationA", k_GetVolumeInformationA},
    {K32, "MoveFileA", k_MoveFileA},
    {K32, "GetTempFileNameA", k_GetTempFileNameA},
    {K32, "GetDiskFreeSpaceA", k_GetDiskFreeSpaceA},
    {K32, "GetFileInformationByHandle", k_GetFileInformationByHandle},
    {K32, "PeekNamedPipe", k_PeekNamedPipe},
    {K32, "GetCurrentDirectoryA", k_GetCurrentDirectoryA},
    {K32, "GetFullPathNameA", k_GetFullPathNameA},
};
const uint32_t rc_k32_file_count = sizeof rc_k32_file / sizeof rc_k32_file[0];
