/* SPDX-License-Identifier: MIT */
/*
 * kernel32: what Windows programs built with MFC use besides the games' set: the lstr*
 * string functions, atoms, resources (FindResource and friends, on the runtime's resource
 * lookup), file times, and small queries.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "k32.h"
#include "rc/hostfs.h"
#include "rc/log.h"
#include "rc/process.h"
#include "rc/vm.h"

/* ---- lstr* ---- */

static void k_lstrcpyA(rc_cpu *c)
{
    uint32_t d = RC_ARG(c, 0), s = RC_ARG(c, 1);
    const char *src = rc_str(s);
    memmove(rc_ptr(d), src, strlen(src) + 1);
    RC_RETURN(c, d, 2);
}

/* lstrcpynA(dest, src, n): at most n - 1 characters and the terminator. */
static void k_lstrcpynA(rc_cpu *c)
{
    uint32_t d = RC_ARG(c, 0), s = RC_ARG(c, 1), n = RC_ARG(c, 2);
    if (n) {
        const char *src = rc_str(s);
        size_t len = strlen(src);
        if (len > n - 1) {
            len = n - 1;
        }
        memmove(rc_ptr(d), src, len);
        rc_st8(d + (uint32_t)len, 0);
    }
    RC_RETURN(c, d, 3);
}

static void k_lstrcatA(rc_cpu *c)
{
    uint32_t d = RC_ARG(c, 0), s = RC_ARG(c, 1);
    const char *src = rc_str(s);
    memmove(rc_ptr(d + (uint32_t)strlen(rc_str(d))), src, strlen(src) + 1);
    RC_RETURN(c, d, 2);
}

static int sign(int v) { return v < 0 ? -1 : v > 0; }

static int compare_nocase(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) {
            return x - y;
        }
    }
}

static void k_lstrcmpA(rc_cpu *c)
{
    const char *a = RC_ARG(c, 0) ? rc_str(RC_ARG(c, 0)) : "", *b = RC_ARG(c, 1) ? rc_str(RC_ARG(c, 1)) : "";
    RC_RETURN(c, (uint32_t)sign(strcmp(a, b)), 2);
}

static void k_lstrcmpiA(rc_cpu *c)
{
    const char *a = RC_ARG(c, 0) ? rc_str(RC_ARG(c, 0)) : "", *b = RC_ARG(c, 1) ? rc_str(RC_ARG(c, 1)) : "";
    RC_RETURN(c, (uint32_t)sign(compare_nocase(a, b)), 2);
}

/* ---- atoms ---- */

#define ATOMS 512
#define ATOM_BASE 0xc000u
static char atoms[ATOMS][256];
static uint32_t atom_refs[ATOMS];

/* An atom for `name`: an integer one ("#123" or below 0xc000), or one from the table. */
static uint32_t atom_find(uint32_t name, bool add)
{
    if (name < 0x10000) {
        return name;
    }
    const char *s = rc_str(name);
    if (s[0] == '#') {
        return (uint32_t)strtoul(s + 1, NULL, 10) & 0xffffu;
    }
    int freei = -1;
    for (int i = 0; i < ATOMS; i++) {
        if (atom_refs[i] && !compare_nocase(atoms[i], s)) {
            atom_refs[i] += add;
            return ATOM_BASE + (uint32_t)i;
        }
        if (!atom_refs[i] && freei < 0) {
            freei = i;
        }
    }
    if (!add || freei < 0) {
        return 0;
    }
    snprintf(atoms[freei], sizeof atoms[freei], "%s", s);
    atom_refs[freei] = 1;
    return ATOM_BASE + (uint32_t)freei;
}

static void k_GlobalAddAtomA(rc_cpu *c) { RC_RETURN(c, atom_find(RC_ARG(c, 0), true), 1); }
static void k_GlobalFindAtomA(rc_cpu *c) { RC_RETURN(c, atom_find(RC_ARG(c, 0), false), 1); }

static void k_GlobalDeleteAtom(rc_cpu *c)
{
    uint32_t a = RC_ARG(c, 0) & 0xffffu;
    if (a >= ATOM_BASE && a < ATOM_BASE + ATOMS && atom_refs[a - ATOM_BASE]) {
        atom_refs[a - ATOM_BASE]--;
    }
    RC_RETURN(c, 0, 1);
}

/* GlobalGetAtomNameA(atom, buffer, size) */
static void k_GlobalGetAtomNameA(rc_cpu *c)
{
    uint32_t a = RC_ARG(c, 0) & 0xffffu;
    char name[260];
    if (a >= ATOM_BASE && a < ATOM_BASE + ATOMS && atom_refs[a - ATOM_BASE]) {
        snprintf(name, sizeof name, "%s", atoms[a - ATOM_BASE]);
    } else if (a && a < ATOM_BASE) {
        snprintf(name, sizeof name, "#%u", a);
    } else {
        RC_RETURN(c, 0, 3);
    }
    RC_RETURN(c, rc_put_str(RC_ARG(c, 1), RC_ARG(c, 2), name), 3);
}

/* ---- resources ----
 * FindResource gives a handle to a small record of where the resource is; LoadResource gives its
 * address (resources are in the mapped image, so they need no loading) and LockResource the same. */

#define RESOURCES 1024
static struct {
    uint32_t data, size;
} res[RESOURCES];
static uint32_t nres;

static bool find_res(uint32_t module, uint32_t name, uint32_t type, uint32_t *data, uint32_t *size)
{
    /* The type: an integer or a name ("#5" is the integer 5). */
    if (type >= 0x10000 && rc_str(type)[0] == '#') {
        type = (uint32_t)strtoul(rc_str(type) + 1, NULL, 10);
    }
    if (name >= 0x10000 && rc_str(name)[0] == '#') {
        name = (uint32_t)strtoul(rc_str(name) + 1, NULL, 10);
    }
    if (type >= 0x10000) {
        return false; /* named types: not used by these programs */
    }
    if (name < 0x10000) {
        return rc_resource(module, type, name, data, size);
    }
    return rc_resource_named(module, type, rc_str(name), data, size);
}

/* FindResourceA(module, name, type) */
static void k_FindResourceA(rc_cpu *c)
{
    uint32_t data, size;
    if (!find_res(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2), &data, &size)) {
        rc_last_error = 1814; /* ERROR_RESOURCE_NAME_NOT_FOUND */
        RC_RETURN(c, 0, 3);
    }
    for (uint32_t i = 0; i < nres; i++) {
        if (res[i].data == data) {
            RC_RETURN(c, 0x7e000000u + 4 * i, 3);
        }
    }
    if (nres == RESOURCES) {
        rc_fatal("kernel32: out of resource handles");
    }
    res[nres].data = data;
    res[nres].size = size;
    RC_RETURN(c, 0x7e000000u + 4 * nres++, 3);
}

/* FindResourceExA(module, type, name, language): resources are found by type and name; the
 * language is the one the program has. */
static void k_FindResourceExA(rc_cpu *c)
{
    uint32_t mod = RC_ARG(c, 0), type = RC_ARG(c, 1), name = RC_ARG(c, 2);
    /* FindResourceA(module, name, type): the same with the arguments in its order */
    c->r[RC_ESP] += 4;
    uint32_t ret = rc_ld32(c->r[RC_ESP] - 4);
    rc_st32(c->r[RC_ESP], ret);
    rc_st32(c->r[RC_ESP] + 4, mod);
    rc_st32(c->r[RC_ESP] + 8, name);
    rc_st32(c->r[RC_ESP] + 12, type);
    k_FindResourceA(c);
}

static uint32_t res_index(uint32_t h) { return h >= 0x7e000000u ? (h - 0x7e000000u) / 4 : RESOURCES; }

/* LoadResource(module, hrsrc) */
static void k_LoadResource(rc_cpu *c)
{
    uint32_t i = res_index(RC_ARG(c, 1));
    RC_RETURN(c, i < nres ? res[i].data : 0, 2);
}

/* SizeofResource(module, hrsrc) */
static void k_SizeofResource(rc_cpu *c)
{
    uint32_t i = res_index(RC_ARG(c, 1));
    RC_RETURN(c, i < nres ? res[i].size : 0, 2);
}

static void k_LockResource(rc_cpu *c) { RC_RETURN(c, RC_ARG(c, 0), 1); }
static void k_FreeResource(rc_cpu *c) { RC_RETURN(c, 0, 1); }

/* ---- files ---- */

/* SetCurrentDirectoryA(path): an existing directory inside the installation directory. */
static void k_SetCurrentDirectoryA(rc_cpu *c)
{
    if (!rc_k32_set_current_dir(rc_str(RC_ARG(c, 0)))) {
        rc_last_error = 3; /* ERROR_PATH_NOT_FOUND */
        RC_RETURN(c, 0, 1);
    }
    RC_RETURN(c, 1, 1);
}

/* RemoveDirectoryA(path): nothing these programs need removed; refused. */
static void k_RemoveDirectoryA(rc_cpu *c)
{
    rc_last_error = 5; /* ERROR_ACCESS_DENIED */
    RC_RETURN(c, 0, 1);
}

/* GetShortPathNameA(long, short, size): long names are fine. */
static void k_GetShortPathNameA(rc_cpu *c)
{
    const char *in = rc_str(RC_ARG(c, 0));
    uint32_t n = RC_ARG(c, 2), len = (uint32_t)strlen(in);
    if (n < len + 1) {
        RC_RETURN(c, len + 1, 3);
    }
    memmove(rc_ptr(RC_ARG(c, 1)), in, len + 1);
    RC_RETURN(c, len, 3);
}

/* LocalFileTimeToFileTime(local, utc): the clock is UTC. */
static void k_LocalFileTimeToFileTime(rc_cpu *c)
{
    memmove(rc_ptr(RC_ARG(c, 1)), rc_ptr(RC_ARG(c, 0)), 8);
    RC_RETURN(c, 1, 2);
}

/* GetFileTime(file, created, accessed, written): a fixed time (2003-01-01). */
static void k_GetFileTime(rc_cpu *c)
{
    for (uint32_t i = 1; i <= 3; i++) {
        if (RC_ARG(c, i)) {
            rc_st32(RC_ARG(c, i), 0x5b8e8000u);
            rc_st32(RC_ARG(c, i) + 4, 0x01c2b131u);
        }
    }
    RC_RETURN(c, 1, 4);
}

static void k_SetFileTime(rc_cpu *c) { RC_RETURN(c, 1, 4); }
static void k_LockFile(rc_cpu *c) { RC_RETURN(c, 1, 5); }
static void k_UnlockFile(rc_cpu *c) { RC_RETURN(c, 1, 5); }

/* DuplicateHandle(process, handle, process, target*, access, inherit, options): the same handle. */
static void k_DuplicateHandle(rc_cpu *c)
{
    if (RC_ARG(c, 3)) {
        rc_st32(RC_ARG(c, 3), RC_ARG(c, 1));
    }
    RC_RETURN(c, 1, 7);
}

/* ---- small queries ---- */

static void k_GetProcessVersion(rc_cpu *c) { RC_RETURN(c, 0x00040000u, 1); }
static void k_GetThreadLocale(rc_cpu *c) { RC_RETURN(c, 0x0409u, 0); }

/* GetProfileStringA(section, key, default, buffer, size): there is no win.ini. */
static void k_GetProfileStringA(rc_cpu *c)
{
    uint32_t def = RC_ARG(c, 2);
    RC_RETURN(c, rc_put_str(RC_ARG(c, 3), RC_ARG(c, 4), def ? rc_str(def) : ""), 5);
}

/* GetProfileIntA(section, key, default): no win.ini, the default. */
static void k_GetProfileIntA(rc_cpu *c) { RC_RETURN(c, RC_ARG(c, 2), 3); }

/* MulDiv(a, b, c): a * b / c rounded, -1 when c is 0. */
static void k_MulDiv(rc_cpu *c)
{
    int64_t a = (int32_t)RC_ARG(c, 0), b = (int32_t)RC_ARG(c, 1), d = (int32_t)RC_ARG(c, 2);
    if (!d) {
        RC_RETURN(c, 0xffffffffu, 3);
    }
    int64_t p = a * b, half = (d < 0 ? -d : d) / 2;
    int64_t r = ((p < 0) != (d < 0)) ? (p - ((p < 0) ? half : -half)) / d : (p + ((p < 0) ? -half : half)) / d;
    RC_RETURN(c, (uint32_t)(int32_t)r, 3);
}

static const rc_import_def defs[] = {
    {K32, "lstrcpyA", k_lstrcpyA},
    {K32, "lstrcpynA", k_lstrcpynA},
    {K32, "lstrcatA", k_lstrcatA},
    {K32, "lstrcmpA", k_lstrcmpA},
    {K32, "lstrcmpiA", k_lstrcmpiA},
    {K32, "GlobalAddAtomA", k_GlobalAddAtomA},
    {K32, "GlobalFindAtomA", k_GlobalFindAtomA},
    {K32, "GlobalDeleteAtom", k_GlobalDeleteAtom},
    {K32, "GlobalGetAtomNameA", k_GlobalGetAtomNameA},
    {K32, "AddAtomA", k_GlobalAddAtomA},
    {K32, "FindAtomA", k_GlobalFindAtomA},
    {K32, "DeleteAtom", k_GlobalDeleteAtom},
    {K32, "GetAtomNameA", k_GlobalGetAtomNameA},
    {K32, "FindResourceA", k_FindResourceA},
    {K32, "FindResourceExA", k_FindResourceExA},
    {K32, "GetProfileIntA", k_GetProfileIntA},
    {K32, "LoadResource", k_LoadResource},
    {K32, "SizeofResource", k_SizeofResource},
    {K32, "LockResource", k_LockResource},
    {K32, "FreeResource", k_FreeResource},
    {K32, "SetCurrentDirectoryA", k_SetCurrentDirectoryA},
    {K32, "RemoveDirectoryA", k_RemoveDirectoryA},
    {K32, "GetShortPathNameA", k_GetShortPathNameA},
    {K32, "LocalFileTimeToFileTime", k_LocalFileTimeToFileTime},
    {K32, "GetFileTime", k_GetFileTime},
    {K32, "SetFileTime", k_SetFileTime},
    {K32, "LockFile", k_LockFile},
    {K32, "UnlockFile", k_UnlockFile},
    {K32, "DuplicateHandle", k_DuplicateHandle},
    {K32, "GetProcessVersion", k_GetProcessVersion},
    {K32, "GetThreadLocale", k_GetThreadLocale},
    {K32, "GetProfileStringA", k_GetProfileStringA},
    {K32, "MulDiv", k_MulDiv},
};

void rc_k32_misc_init(void) { rc_register_imports(defs, sizeof defs / sizeof defs[0]); }
