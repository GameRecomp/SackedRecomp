/* SPDX-License-Identifier: MIT */
/* Reserved import addresses, the registry of HLE functions, and tracing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc/dispatch.h"
#include "rc/hle.h"
#include "rc/log.h"

#define IMPORT_BASE 0xf0000000u
#define IMPORT_STRIDE 16u
#define IMPORT_MAX 4096u
#define MODULES_MAX 64u

typedef struct slot {
    const char *dll;
    const char *name;
    rc_fn fn;
} slot;

static slot slots[IMPORT_MAX];
static uint32_t nslots;

static struct {
    const rc_import_def *defs;
    uint32_t count;
} modules[MODULES_MAX];
static uint32_t nmodules;

static bool trace;
static uint32_t current; /* the import being called, for the generic stubs */

uint32_t rc_last_error;

static int ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') {
            x += 32;
        }
        if (y >= 'A' && y <= 'Z') {
            y += 32;
        }
        if (x != y) {
            return 0;
        }
        if (!x) {
            return 1;
        }
    }
}

/* "kernel32" and "KERNEL32.dll" are the same DLL. */
static int dll_eq(const char *a, const char *b)
{
    char x[64], y[64];
    snprintf(x, sizeof x, "%s", a);
    snprintf(y, sizeof y, "%s", b);
    char *d = strrchr(x, '.');
    if (d && ieq(d, ".dll")) {
        *d = 0;
    }
    d = strrchr(y, '.');
    if (d && ieq(d, ".dll")) {
        *d = 0;
    }
    return ieq(x, y);
}

static rc_fn find(const char *dll, const char *name)
{
    for (uint32_t m = 0; m < nmodules; m++) {
        for (uint32_t i = 0; i < modules[m].count; i++) {
            const rc_import_def *d = &modules[m].defs[i];
            if (dll_eq(d->dll, dll) && strcmp(d->name, name) == 0) {
                return d->fn;
            }
        }
    }
    return NULL;
}

bool rc_import_implemented(const char *dll, const char *name)
{
    return find(dll, name) != NULL || rc_data_address(dll, name) != 0;
}

/* Variables HLE modules export. */
#define DATA_MAX 64u
static struct {
    const char *dll, *name;
    rc_data_fn addr;
} data_exports[DATA_MAX];
static uint32_t ndata;

void rc_register_data(const char *dll, const char *name, rc_data_fn addr)
{
    if (ndata == DATA_MAX) {
        rc_fatal("too many HLE variables");
    }
    data_exports[ndata].dll = dll;
    data_exports[ndata].name = name;
    data_exports[ndata].addr = addr;
    ndata++;
}

uint32_t rc_data_address(const char *dll, const char *name)
{
    for (uint32_t i = 0; i < ndata; i++) {
        if (dll_eq(data_exports[i].dll, dll) && strcmp(data_exports[i].name, name) == 0) {
            return data_exports[i].addr();
        }
    }
    return 0;
}

static void unimplemented(rc_cpu *c)
{
    const slot *s = &slots[current];
    rc_fatal("unimplemented import %s!%s, called from %#010x", s->dll, s->name,
             rc_ld32(c->r[RC_ESP]));
}

static void traced(rc_cpu *c)
{
    const slot *s = &slots[current];
    uint32_t sp = c->r[RC_ESP];
    fprintf(stderr, "[import] %s!%s(%#x, %#x, %#x, %#x) from %#010x\n", s->dll, s->name,
            rc_ld32(sp + 4), rc_ld32(sp + 8), rc_ld32(sp + 12), rc_ld32(sp + 16), rc_ld32(sp));
    s->fn(c);
    fprintf(stderr, "[import]   = %#x\n", c->r[RC_EAX]);
}

/* RC_STOP_AT=DLL!name: end the run, successfully, at the first call of an
 * import (scripted runs use it as their goal). */
static void stop_check(const slot *s)
{
    static const char *stop;
    static int read;
    if (!read) {
        stop = getenv("RC_STOP_AT");
        read = 1;
    }
    if (!stop) {
        return;
    }
    const char *bang = strchr(stop, '!');
    if (bang && strcmp(bang + 1, s->name) == 0) {
        char dll[64];
        snprintf(dll, sizeof dll, "%.*s", (int)(bang - stop), stop);
        if (dll_eq(dll, s->dll)) {
            fprintf(stderr, "reached %s!%s\n", s->dll, s->name);
            fflush(stdout);
            fflush(stderr);
            exit(0);
        }
    }
}

static rc_fn resolve(uint32_t addr)
{
    if (!rc_is_import_address(addr)) {
        return NULL;
    }
    current = (addr - IMPORT_BASE) / IMPORT_STRIDE;
    stop_check(&slots[current]);
    if (slots[current].fn == NULL) {
        return unimplemented;
    }
    return trace ? traced : slots[current].fn;
}

void rc_register_imports(const rc_import_def *defs, uint32_t count)
{
    if (nmodules == MODULES_MAX) {
        rc_fatal("too many HLE modules");
    }
    modules[nmodules].defs = defs;
    modules[nmodules].count = count;
    nmodules++;
    rc_set_import_resolver(resolve);
    /* Slots handed out before this registration may now have a function. */
    for (uint32_t i = 0; i < nslots; i++) {
        if (slots[i].fn == NULL) {
            slots[i].fn = find(slots[i].dll, slots[i].name);
        }
    }
}

/* The slots by DLL and name: HLE code asks for addresses at run time (msvcrt for each ReadFile
 * of an fread), and a search through every slot, comparing names, took most of a Sacked level's
 * time. The hash is of what dll_eq and strcmp compare: the DLL without ".dll", in lower case,
 * and the name as it is. */
#define HASH_SIZE (IMPORT_MAX * 2u)
static uint32_t by_name[HASH_SIZE]; /* slot + 1, 0 for none */

static uint32_t name_hash(const char *dll, const char *name)
{
    uint32_t h = 2166136261u;
    size_t n = strlen(dll);
    if (n >= 4 && ieq(dll + n - 4, ".dll")) {
        n -= 4;
    }
    for (size_t i = 0; i < n; i++) {
        int x = (unsigned char)dll[i];
        h = (h ^ (uint32_t)(x >= 'A' && x <= 'Z' ? x + 32 : x)) * 16777619u;
    }
    h = (h ^ 0x100u) * 16777619u;
    for (const char *p = name; *p; p++) {
        h = (h ^ (unsigned char)*p) * 16777619u;
    }
    return h;
}

uint32_t rc_import_address(const char *dll, const char *name)
{
    uint32_t h = name_hash(dll, name) & (HASH_SIZE - 1);
    for (; by_name[h]; h = (h + 1) & (HASH_SIZE - 1)) {
        uint32_t i = by_name[h] - 1;
        if (strcmp(slots[i].name, name) == 0 && dll_eq(slots[i].dll, dll)) {
            return IMPORT_BASE + IMPORT_STRIDE * i;
        }
    }
    if (nslots == IMPORT_MAX) {
        rc_fatal("too many imports");
    }
    by_name[h] = nslots + 1;
    /* The names outlive the call: they come from the image or the registry. */
    slots[nslots].dll = dll;
    slots[nslots].name = name;
    slots[nslots].fn = find(dll, name);
    rc_set_import_resolver(resolve);
    return IMPORT_BASE + IMPORT_STRIDE * nslots++;
}

bool rc_is_import_address(uint32_t addr)
{
    return addr >= IMPORT_BASE && addr < IMPORT_BASE + IMPORT_STRIDE * nslots &&
           (addr - IMPORT_BASE) % IMPORT_STRIDE == 0;
}

void rc_set_import_trace(bool on) { trace = on; }

uint32_t rc_put_str(uint32_t dst, uint32_t cap, const char *s)
{
    if (cap == 0) {
        return 0;
    }
    uint32_t n = (uint32_t)strlen(s);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(rc_ptr(dst), s, n);
    rc_st8(dst + n, 0);
    return n;
}

/* Where a guest function called from the host returns to. Lifted code
 * pops it and returns to C; it is never executed. */
#define CALLBACK_RETURN 0xeffffff0u

uint32_t rc_call_guest(rc_cpu *c, uint32_t fn, uint32_t nargs, const uint32_t *args)
{
    for (uint32_t i = nargs; i-- > 0;) {
        rc_push32(c, args[i]);
    }
    rc_push32(c, CALLBACK_RETURN);
    rc_dispatch(c, fn);
    return c->r[RC_EAX];
}
