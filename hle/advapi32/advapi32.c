/* SPDX-License-Identifier: MIT */
/*
 * ADVAPI32: the registry, and the user name.
 *
 * The registry is the program's own, kept in registry.txt in the user directory: what the
 * game writes is there on the next start, nothing of the host's is visible. A key is a path
 * below a root ("HKLM\Software\..."), compared without case as Windows does; values are
 * strings, DWORDs or binary. A key that was never created does not exist, as on a clean
 * install, so a game falls back to its defaults (or creates it).
 *
 * The file is plain text, one line per value:  KEY<TAB>NAME<TAB>TYPE<TAB>HEX. What the game's
 * installer wrote comes first, from registry-install.txt in the game directory (`recomp extract`
 * makes it), as the registry would be after installing.
 */
#include "advapi32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc/hostfs.h"
#include "rc/log.h"
#include "rc/process.h"

#define ADV "ADVAPI32.dll"

#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_MORE_DATA 234u
#define ERROR_NO_MORE_ITEMS 259u
#define REG_CREATED_NEW_KEY 1u
#define REG_OPENED_EXISTING_KEY 2u

#define KEYS 256
#define VALUES 1024
#define HANDLES 64
#define HANDLE_BASE 0x00c0de00u

typedef struct value {
    int key;
    char name[64];
    uint32_t type;
    uint32_t size;
    uint8_t *data;
} value;

static char keys[KEYS][256];
static int nkeys;
static value values[VALUES];
static int nvalues;
static int handles[HANDLES]; /* key index + 1, 0 = free */
static bool loaded;

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a - 'A' + 'a') : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b - 'A' + 'a') : *b;
        if (x != y) {
            return 0;
        }
    }
    return *a == *b;
}

static int find_key(const char *path)
{
    for (int i = 0; i < nkeys; i++) {
        if (ieq(keys[i], path)) {
            return i;
        }
    }
    return -1;
}

/* The key and, with `create`, every key above it. */
static int add_key(const char *path)
{
    int k = find_key(path);
    if (k >= 0 || nkeys == KEYS) {
        return k;
    }
    char parent[256];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '\\');
    if (slash && strchr(parent, '\\') != slash) {
        *slash = 0;
        add_key(parent);
    }
    snprintf(keys[nkeys], sizeof keys[nkeys], "%s", path);
    return nkeys++;
}

static value *find_value(int key, const char *name)
{
    for (int i = 0; i < nvalues; i++) {
        if (values[i].key == key && ieq(values[i].name, name)) {
            return &values[i];
        }
    }
    return NULL;
}

static void set_value(int key, const char *name, uint32_t type, const uint8_t *data, uint32_t size)
{
    value *v = find_value(key, name);
    if (!v) {
        if (nvalues == VALUES) {
            return;
        }
        v = &values[nvalues++];
        v->key = key;
        snprintf(v->name, sizeof v->name, "%s", name);
        v->data = NULL;
    }
    free(v->data);
    v->type = type;
    v->size = size;
    v->data = malloc(size ? size : 1);
    if (v->data && size) {
        memcpy(v->data, data, size);
    }
}

static bool file_path(char *out)
{
    return rc_cfg && rc_cfg->user_dir && rc_host_resolve(rc_cfg->user_dir, "registry.txt", true, out);
}

/* One registry file. `install`: the installer's entries, whose strings may hold `{app}`, the
 * installation directory as the program sees it, and `{drive:{app}}`, its drive. */
static void load_file(const char *path, bool install)
{
    /* Read whole through rc_host_read: in a browser the game's files are read only that way. */
    uint32_t len = 0;
    uint8_t *text = rc_host_read(path, &len);
    if (!text) {
        return;
    }
    text[len] = 0;
    char line[4096];
    for (const char *at = (const char *)text; *at;) {
        const char *eol = strchr(at, '\n');
        size_t used = eol ? (size_t)(eol - at) + 1 : strlen(at);
        size_t keep = used < sizeof line - 1 ? used : sizeof line - 1;
        memcpy(line, at, keep);
        line[keep] = 0;
        at += used;
        char *k = line, *n = strchr(k, '\t');
        char *t = n ? strchr(n + 1, '\t') : NULL;
        char *h = t ? strchr(t + 1, '\t') : NULL;
        if (!h) {
            continue;
        }
        *n++ = 0;
        *t++ = 0;
        *h++ = 0;
        int key = add_key(k);
        if (key < 0) {
            continue;
        }
        if (*n == 0 && *t == '-') {
            continue; /* an empty key */
        }
        uint8_t data[2048];
        uint32_t size = 0;
        while (h[0] && h[1] && h[0] != '\n' && size < sizeof data) {
            unsigned b;
            if (sscanf(h, "%2x", &b) != 1) {
                break;
            }
            data[size++] = (uint8_t)b;
            h += 2;
        }
        uint32_t type = (uint32_t)strtoul(t, NULL, 10);
        if (install && (type == 1 || type == 2) && size && rc_cfg && rc_cfg->guest_dir) {
            /* {app} → the installation directory (strings only; the data ends with its NUL) */
            char str[2048], out[2048];
            memcpy(str, data, size);
            str[size - 1] = 0;
            size_t o = 0;
            for (const char *p = str; *p && o + 1 < sizeof out;) {
                if (!strncmp(p, "{app}", 5)) {
                    o += (size_t)snprintf(out + o, sizeof out - o, "%s", rc_cfg->guest_dir);
                    o = o < sizeof out ? o : sizeof out - 1;
                    p += 5;
                } else if (!strncmp(p, "{drive:{app}}", 13)) {
                    /* the installation directory's drive, "C:" */
                    const char *g = rc_cfg->guest_dir;
                    size_t dn = g[0] && g[1] == ':' ? 2 : 0;
                    o += (size_t)snprintf(out + o, sizeof out - o, "%.*s", (int)dn, g);
                    o = o < sizeof out ? o : sizeof out - 1;
                    p += 13;
                } else {
                    out[o++] = *p++;
                }
            }
            out[o] = 0;
            size = (uint32_t)o + 1;
            memcpy(data, out, size);
        }
        set_value(key, n, type, data, size);
    }
    free(text);
}

/* The installer's entries (registry-install.txt in the game directory, written when it was
 * unpacked), then what the program wrote before (registry.txt in the user directory). */
static void load(void)
{
    if (loaded) {
        return;
    }
    loaded = true;
    char path[RC_PATH_MAX];
    if (rc_cfg && rc_cfg->game_dir && rc_host_resolve(rc_cfg->game_dir, "registry-install.txt", false, path)) {
        load_file(path, true);
    }
    if (file_path(path)) {
        load_file(path, false);
    }
}

static void save(void)
{
    char path[RC_PATH_MAX];
    FILE *f = file_path(path) ? fopen(path, "w") : NULL;
    if (!f) {
        return;
    }
    for (int k = 0; k < nkeys; k++) {
        if (!keys[k][0]) {
            continue; /* deleted */
        }
        bool any = false;
        for (int i = 0; i < nvalues; i++) {
            if (values[i].key != k) {
                continue;
            }
            any = true;
            fprintf(f, "%s\t%s\t%u\t", keys[k], values[i].name, values[i].type);
            for (uint32_t j = 0; j < values[i].size; j++) {
                fprintf(f, "%02x", values[i].data[j]);
            }
            fputc('\n', f);
        }
        if (!any) {
            fprintf(f, "%s\t\t-\t\n", keys[k]);
        }
    }
    fclose(f);
}

/* The path of `sub` below the handle or root `h`; false for an unknown handle. */
static bool path_of(uint32_t h, const char *sub, char *out, size_t cap)
{
    const char *root = NULL;
    switch (h) {
    case 0x80000000u: root = "HKCR"; break;
    case 0x80000001u: root = "HKCU"; break;
    case 0x80000002u: root = "HKLM"; break;
    case 0x80000003u: root = "HKU"; break;
    case 0x80000005u: root = "HKCC"; break;
    default:
        if (h >= HANDLE_BASE && h < HANDLE_BASE + HANDLES && handles[h - HANDLE_BASE]) {
            root = keys[handles[h - HANDLE_BASE] - 1];
        }
        break;
    }
    if (!root) {
        return false;
    }
    if (sub && *sub) {
        snprintf(out, cap, "%s\\%s", root, sub);
    } else {
        snprintf(out, cap, "%s", root);
    }
    size_t n = strlen(out);
    while (n && out[n - 1] == '\\') {
        out[--n] = 0;
    }
    return true;
}

static uint32_t new_handle(int key)
{
    for (int i = 0; i < HANDLES; i++) {
        if (!handles[i]) {
            handles[i] = key + 1;
            return HANDLE_BASE + (uint32_t)i;
        }
    }
    return 0;
}

static int key_of(uint32_t h)
{
    char path[256];
    return path_of(h, NULL, path, sizeof path) ? find_key(path) : -1;
}

/* RegOpenKeyExA(key, sub, options, sam, result) */
static void k_RegOpenKeyExA(rc_cpu *c)
{
    load();
    char path[256];
    if (!path_of(RC_ARG(c, 0), rc_str(RC_ARG(c, 1)), path, sizeof path)) {
        RC_RETURN(c, ERROR_INVALID_HANDLE, 5);
    }
    int k = find_key(path);
    if (rc_trace("registry")) {
        rc_log("[registry] open %s: %s", path, k < 0 ? "not found" : "ok");
    }
    if (k < 0) {
        RC_RETURN(c, ERROR_FILE_NOT_FOUND, 5);
    }
    rc_st32(RC_ARG(c, 4), new_handle(k));
    RC_RETURN(c, ERROR_SUCCESS, 5);
}

/* RegCreateKeyExA(key, sub, reserved, class, options, sam, security, result, disposition) */
static void k_RegCreateKeyExA(rc_cpu *c)
{
    load();
    char path[256];
    if (!path_of(RC_ARG(c, 0), rc_str(RC_ARG(c, 1)), path, sizeof path)) {
        RC_RETURN(c, ERROR_INVALID_HANDLE, 9);
    }
    bool existed = find_key(path) >= 0;
    int k = add_key(path);
    if (k < 0) {
        RC_RETURN(c, ERROR_FILE_NOT_FOUND, 9);
    }
    if (!existed) {
        save();
    }
    rc_st32(RC_ARG(c, 7), new_handle(k));
    if (RC_ARG(c, 8)) {
        rc_st32(RC_ARG(c, 8), existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY);
    }
    RC_RETURN(c, ERROR_SUCCESS, 9);
}

static void k_RegCloseKey(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0);
    if (h >= HANDLE_BASE && h < HANDLE_BASE + HANDLES) {
        handles[h - HANDLE_BASE] = 0;
    }
    RC_RETURN(c, ERROR_SUCCESS, 1);
}

/* RegQueryValueExA(key, name, reserved, type, data, size) */
static void k_RegQueryValueExA(rc_cpu *c)
{
    load();
    int k = key_of(RC_ARG(c, 0));
    if (k < 0) {
        RC_RETURN(c, ERROR_INVALID_HANDLE, 6);
    }
    const char *name = rc_str(RC_ARG(c, 1));
    value *v = find_value(k, name ? name : "");
    if (rc_trace("registry")) {
        rc_log("[registry] query %s \\ %s: %s", keys[k], name ? name : "", v ? "ok" : "not found");
    }
    if (!v) {
        RC_RETURN(c, ERROR_FILE_NOT_FOUND, 6);
    }
    uint32_t ptype = RC_ARG(c, 3), data = RC_ARG(c, 4), psize = RC_ARG(c, 5);
    if (ptype) {
        rc_st32(ptype, v->type);
    }
    uint32_t r = ERROR_SUCCESS;
    if (psize) {
        uint32_t cap = rc_ld32(psize);
        if (data && cap < v->size) {
            r = ERROR_MORE_DATA;
        } else if (data) {
            memcpy(rc_ptr(data), v->data, v->size);
        }
        rc_st32(psize, v->size);
    }
    RC_RETURN(c, r, 6);
}

/* RegSetValueExA(key, name, reserved, type, data, size) */
static void k_RegSetValueExA(rc_cpu *c)
{
    load();
    int k = key_of(RC_ARG(c, 0));
    if (k < 0) {
        RC_RETURN(c, ERROR_INVALID_HANDLE, 6);
    }
    const char *name = rc_str(RC_ARG(c, 1));
    uint32_t size = RC_ARG(c, 5);
    set_value(k, name ? name : "", RC_ARG(c, 3), RC_ARG(c, 4) ? (const uint8_t *)rc_ptr(RC_ARG(c, 4)) : NULL,
              RC_ARG(c, 4) ? size : 0);
    save();
    RC_RETURN(c, ERROR_SUCCESS, 6);
}

/* RegDeleteValueA(key, name) */
static void k_RegDeleteValueA(rc_cpu *c)
{
    load();
    int k = key_of(RC_ARG(c, 0));
    const char *name = rc_str(RC_ARG(c, 1));
    value *v = k >= 0 ? find_value(k, name ? name : "") : NULL;
    if (!v) {
        RC_RETURN(c, ERROR_FILE_NOT_FOUND, 2);
    }
    free(v->data);
    *v = values[--nvalues];
    save();
    RC_RETURN(c, ERROR_SUCCESS, 2);
}

/* ---- the Windows 3.1 calls (MFC's profile and registration code uses them) ---- */

/* RegOpenKeyA(key, sub, result) */
static void k_RegOpenKeyA(rc_cpu *c)
{
    load();
    char path[256];
    if (!path_of(RC_ARG(c, 0), RC_ARG(c, 1) ? rc_str(RC_ARG(c, 1)) : NULL, path, sizeof path)) {
        RC_RETURN(c, ERROR_INVALID_HANDLE, 3);
    }
    int k = find_key(path);
    if (k < 0) {
        RC_RETURN(c, ERROR_FILE_NOT_FOUND, 3);
    }
    rc_st32(RC_ARG(c, 2), new_handle(k));
    RC_RETURN(c, ERROR_SUCCESS, 3);
}

/* RegCreateKeyA(key, sub, result) */
static void k_RegCreateKeyA(rc_cpu *c)
{
    load();
    char path[256];
    if (!path_of(RC_ARG(c, 0), RC_ARG(c, 1) ? rc_str(RC_ARG(c, 1)) : NULL, path, sizeof path)) {
        RC_RETURN(c, ERROR_INVALID_HANDLE, 3);
    }
    bool existed = find_key(path) >= 0;
    int k = add_key(path);
    if (k < 0) {
        RC_RETURN(c, ERROR_FILE_NOT_FOUND, 3);
    }
    if (!existed) {
        save();
    }
    rc_st32(RC_ARG(c, 2), new_handle(k));
    RC_RETURN(c, ERROR_SUCCESS, 3);
}

/* RegSetValueA(key, sub, type, text, size): the unnamed value of the subkey, a string. */
static void k_RegSetValueA(rc_cpu *c)
{
    load();
    char path[256];
    if (!path_of(RC_ARG(c, 0), RC_ARG(c, 1) ? rc_str(RC_ARG(c, 1)) : NULL, path, sizeof path)) {
        RC_RETURN(c, ERROR_INVALID_HANDLE, 5);
    }
    int k = add_key(path);
    if (k < 0) {
        RC_RETURN(c, ERROR_FILE_NOT_FOUND, 5);
    }
    const char *text = RC_ARG(c, 3) ? rc_str(RC_ARG(c, 3)) : "";
    set_value(k, "", 1 /* REG_SZ */, (const uint8_t *)text, (uint32_t)strlen(text) + 1);
    save();
    RC_RETURN(c, ERROR_SUCCESS, 5);
}

/* RegQueryValueA(key, sub, text, size*): the unnamed value of the subkey. */
static void k_RegQueryValueA(rc_cpu *c)
{
    load();
    char path[256];
    if (!path_of(RC_ARG(c, 0), RC_ARG(c, 1) ? rc_str(RC_ARG(c, 1)) : NULL, path, sizeof path)) {
        RC_RETURN(c, ERROR_INVALID_HANDLE, 4);
    }
    int k = find_key(path);
    value *v = k >= 0 ? find_value(k, "") : NULL;
    if (k < 0) {
        RC_RETURN(c, ERROR_FILE_NOT_FOUND, 4);
    }
    uint32_t data = RC_ARG(c, 2), psize = RC_ARG(c, 3), size = v ? v->size : 1;
    if (psize) {
        uint32_t cap = rc_ld32(psize);
        rc_st32(psize, size);
        if (data && cap < size) {
            RC_RETURN(c, ERROR_MORE_DATA, 4);
        }
    }
    if (data) {
        if (v) {
            memcpy(rc_ptr(data), v->data, v->size);
        } else {
            rc_st8(data, 0);
        }
    }
    RC_RETURN(c, ERROR_SUCCESS, 4);
}

/* The name of the `index`th key directly below `parent`, or NULL. */
static const char *child_key(int parent, uint32_t index)
{
    size_t pl = strlen(keys[parent]);
    for (int i = 0; i < nkeys; i++) {
        const char *k = keys[i];
        if (i != parent && strlen(k) > pl + 1 && k[pl] == '\\' && !strchr(k + pl + 1, '\\')) {
            char head[256];
            snprintf(head, sizeof head, "%.*s", (int)pl, k);
            if (ieq(head, keys[parent]) && index-- == 0) {
                return k + pl + 1;
            }
        }
    }
    return NULL;
}

/* RegEnumKeyA(key, index, name, size) */
static void k_RegEnumKeyA(rc_cpu *c)
{
    load();
    int k = key_of(RC_ARG(c, 0));
    const char *name = k >= 0 ? child_key(k, RC_ARG(c, 1)) : NULL;
    if (!name) {
        RC_RETURN(c, k < 0 ? ERROR_INVALID_HANDLE : 259 /* ERROR_NO_MORE_ITEMS */, 4);
    }
    if (strlen(name) + 1 > RC_ARG(c, 3)) {
        RC_RETURN(c, ERROR_MORE_DATA, 4);
    }
    rc_put_str(RC_ARG(c, 2), RC_ARG(c, 3), name);
    RC_RETURN(c, ERROR_SUCCESS, 4);
}

/* RegEnumKeyExA(key, index, name, size*, reserved, class, class size*, time) */
static void k_RegEnumKeyExA(rc_cpu *c)
{
    load();
    int k = key_of(RC_ARG(c, 0));
    const char *name = k >= 0 ? child_key(k, RC_ARG(c, 1)) : NULL;
    if (!name) {
        RC_RETURN(c, k < 0 ? ERROR_INVALID_HANDLE : 259, 8);
    }
    uint32_t cap = rc_ld32(RC_ARG(c, 3)), len = (uint32_t)strlen(name);
    if (len + 1 > cap) {
        RC_RETURN(c, ERROR_MORE_DATA, 8);
    }
    rc_put_str(RC_ARG(c, 2), cap, name);
    rc_st32(RC_ARG(c, 3), len);
    RC_RETURN(c, ERROR_SUCCESS, 8);
}

/* RegDeleteKeyA(key, sub): the key, its values and the keys below it (their names are cleared). */
static void k_RegDeleteKeyA(rc_cpu *c)
{
    load();
    char path[256];
    if (!path_of(RC_ARG(c, 0), RC_ARG(c, 1) ? rc_str(RC_ARG(c, 1)) : NULL, path, sizeof path)) {
        RC_RETURN(c, ERROR_INVALID_HANDLE, 2);
    }
    if (find_key(path) < 0) {
        RC_RETURN(c, ERROR_FILE_NOT_FOUND, 2);
    }
    size_t pl = strlen(path);
    for (int i = 0; i < nkeys; i++) {
        char head[256];
        snprintf(head, sizeof head, "%.*s", (int)pl, keys[i]);
        if (ieq(head, path) && (keys[i][pl] == 0 || keys[i][pl] == '\\')) {
            for (int v = 0; v < nvalues;) {
                if (values[v].key == i) {
                    free(values[v].data);
                    values[v] = values[--nvalues];
                } else {
                    v++;
                }
            }
            keys[i][0] = 0;
        }
    }
    save();
    RC_RETURN(c, ERROR_SUCCESS, 2);
}

/* GetFileSecurityA / SetFileSecurityA: there is no security to copy (MFC copes). */
static void k_GetFileSecurityA(rc_cpu *c)
{
    rc_last_error = 50; /* ERROR_NOT_SUPPORTED */
    RC_RETURN(c, 0, 5);
}
static void k_SetFileSecurityA(rc_cpu *c)
{
    rc_last_error = 50;
    RC_RETURN(c, 0, 3);
}

/* GetUserNameA(buffer, size) */
static void k_GetUserNameA(rc_cpu *c)
{
    const char *user = "Player";
    uint32_t buf = RC_ARG(c, 0), psize = RC_ARG(c, 1);
    uint32_t need = (uint32_t)strlen(user) + 1;
    if (!psize || rc_ld32(psize) < need) {
        if (psize) {
            rc_st32(psize, need);
        }
        RC_RETURN(c, 0, 2);
    }
    rc_put_str(buf, need, user);
    rc_st32(psize, need);
    RC_RETURN(c, 1, 2);
}

const rc_import_def rc_advapi32_defs[] = {
    {ADV, "RegOpenKeyExA", k_RegOpenKeyExA},
    {ADV, "RegCreateKeyExA", k_RegCreateKeyExA},
    {ADV, "RegCloseKey", k_RegCloseKey},
    {ADV, "RegQueryValueExA", k_RegQueryValueExA},
    {ADV, "RegSetValueExA", k_RegSetValueExA},
    {ADV, "RegDeleteValueA", k_RegDeleteValueA},
    {ADV, "GetUserNameA", k_GetUserNameA},
    {ADV, "RegOpenKeyA", k_RegOpenKeyA},
    {ADV, "RegCreateKeyA", k_RegCreateKeyA},
    {ADV, "RegSetValueA", k_RegSetValueA},
    {ADV, "RegQueryValueA", k_RegQueryValueA},
    {ADV, "RegEnumKeyA", k_RegEnumKeyA},
    {ADV, "RegEnumKeyExA", k_RegEnumKeyExA},
    {ADV, "RegDeleteKeyA", k_RegDeleteKeyA},
    {ADV, "GetFileSecurityA", k_GetFileSecurityA},
    {ADV, "SetFileSecurityA", k_SetFileSecurityA},
};
const uint32_t rc_advapi32_count = sizeof rc_advapi32_defs / sizeof rc_advapi32_defs[0];
