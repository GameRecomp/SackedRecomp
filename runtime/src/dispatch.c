/* SPDX-License-Identifier: MIT */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "rc/dispatch.h"
#include "rc/process.h"

#include <time.h>

/* The lifted code: one sorted table per binary (a game links several, each
 * with its own; rc_set_lifted installs one). */
#define TABLES 16
static struct {
    const rc_fn_entry *entries;
    uint32_t count;
} tables[TABLES];
static uint32_t ntables;
static rc_import_resolver import_resolver;
static void (*call_hook)(rc_cpu *c, uint32_t target);

void rc_set_call_hook(void (*hook)(rc_cpu *c, uint32_t target)) { call_hook = hook; }

void rc_set_lifted(const rc_fn_entry *t, uint32_t count)
{
    ntables = 0;
    rc_add_lifted(t, count);
}

bool rc_add_lifted(const rc_fn_entry *t, uint32_t count)
{
    if (ntables == TABLES) {
        return false;
    }
    tables[ntables].entries = t;
    tables[ntables].count = count;
    ntables++;
    return true;
}

void rc_set_import_resolver(rc_import_resolver r) { import_resolver = r; }

/* Patches: few, so a short list searched in order. */
#define HOOKS 32
static struct {
    uint32_t addr;
    rc_fn fn;
} hooks[HOOKS];
static uint32_t nhooks;

rc_fn rc_hook(uint32_t addr, rc_fn wrapper)
{
    for (uint32_t i = 0; i < nhooks; i++) {
        if (hooks[i].addr == addr) {
            rc_fn before = hooks[i].fn;
            hooks[i].fn = wrapper;
            return before;
        }
    }
    if (nhooks == HOOKS) {
        rc_fatal("too many patches");
    }
    hooks[nhooks].addr = addr;
    hooks[nhooks].fn = wrapper;
    nhooks++;
    return NULL;
}

static struct {
    uint32_t addr;
    rc_fn fn;
} points[HOOKS];
static uint32_t npoints;

void rc_on_point(uint32_t addr, rc_fn fn)
{
    for (uint32_t i = 0; i < npoints; i++) {
        if (points[i].addr == addr) {
            points[i].fn = fn;
            return;
        }
    }
    if (npoints == HOOKS) {
        rc_fatal("too many patch points");
    }
    points[npoints].addr = addr;
    points[npoints].fn = fn;
    npoints++;
}

void rc_point(rc_cpu *c, uint32_t addr)
{
    for (uint32_t i = 0; i < npoints; i++) {
        if (points[i].addr == addr) {
            c->eip = addr; /* which point: one function may serve the builds of a game */
            points[i].fn(c);
            return;
        }
    }
}

rc_fn rc_lookup(uint32_t addr)
{
    for (uint32_t i = 0; i < nhooks; i++) {
        if (hooks[i].addr == addr) {
            return hooks[i].fn;
        }
    }
    return rc_lookup_lifted(addr);
}

rc_fn rc_lookup_lifted(uint32_t addr)
{
    for (uint32_t k = 0; k < ntables; k++) {
        const rc_fn_entry *table = tables[k].entries;
        uint32_t lo = 0, hi = tables[k].count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (table[mid].addr < addr) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo < tables[k].count && table[lo].addr == addr) {
            return table[lo].fn;
        }
    }
    return NULL;
}

void rc_dispatch_jump(rc_cpu *c, uint32_t target)
{
    if (c->eh_depth && c->r[RC_ESP] > c->eh_esp) {
        c->unwind = 1;
        c->unwind_eip = target;
        c->unwind_ebp = c->r[RC_EBP];
        c->eip = target;
        return;
    }
    rc_dispatch(c, target);
}

void rc_unwind_claim(rc_cpu *c, uint32_t ebp, uint32_t lo, uint32_t hi, const uint32_t *at, uint32_t n)
{
    if (c->unwind_ebp != ebp) {
        return;
    }
    bool mine = c->unwind_eip >= lo && c->unwind_eip < hi;
    for (uint32_t i = 0; i < n && !mine; i++) {
        mine = at[i] == c->unwind_eip;
    }
    if (!mine) {
        return;
    }
    c->unwind = 0;
    c->resume_eip = c->unwind_eip;
    rc_dispatch(c, c->unwind_eip);
    c->resume_eip = 0;
}

void rc_unwind_begin(rc_cpu *c, uint32_t eip)
{
    c->unwind = 1;
    c->unwind_eip = eip;
    c->unwind_ebp = c->r[RC_EBP];
    c->eip = eip;
}

void rc_dispatch(rc_cpu *c, uint32_t target)
{
    if (call_hook) {
        call_hook(c, target);
    }
    rc_fn f = rc_lookup(target);
    if (f == NULL && import_resolver != NULL) {
        f = import_resolver(target);
    }
    if (f == NULL) {
        c->eip = target;
        rc_fatal("indirect transfer to %#010x, which is not a lifted function "
                 "(esp %#010x, return address %#010x)",
                 target, c->r[RC_ESP], rc_ld32(c->r[RC_ESP]));
    }
    f(c);
}

void rc_trap(rc_cpu *c, uint32_t addr, const char *why)
{
    c->eip = addr;
    rc_fatal("%s (at %#010x; eax %#x ecx %#x edx %#x ebx %#x esp %#x ebp %#x esi %#x edi %#x)", why,
             addr, c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]);
}

/* A fixed CPU: a P6-class Intel processor with FPU, TSC, CX8, CMOV and MMX
 * (a Pentium II), and no SSE. MMX is lifted (rc/mmx.h, checked against the
 * real CPU by the difftest), and programs of the time rely on it: Cultures 1
 * draws its players' sprites in 16-bit modes only when CPUID reports MMX.
 * SSE stays off, so libraries take their MMX or plain code paths. */
void rc_cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    switch (*eax) {
    case 0:
        *eax = 1;
        *ebx = 0x756e6547; /* "Genu" */
        *edx = 0x49656e69; /* "ineI" */
        *ecx = 0x6c65746e; /* "ntel" */
        break;
    case 1:
        *eax = 0x00000683; /* family 6, model 8, stepping 3 */
        *ebx = 0;
        *ecx = 0;
        *edx = 0x00808111; /* FPU, TSC, CX8, CMOV, MMX (bit 23) */
        break;
    default:
        *eax = *ebx = *ecx = *edx = 0;
        break;
    }
}

/* Nanoseconds since an arbitrary start: a 1 GHz time-stamp counter. */
void rc_rdtsc(uint32_t *eax, uint32_t *edx)
{
    struct timespec ts;
    uint64_t t = 0;
    if (rc_clock_virtual()) {
        /* On the virtual clock its time, and a step per reading so that it
         * always moves on: a run repeats. */
        static uint64_t reads;
        t = rc_ticks_peek_ms() * 1000000u + (++reads % 1000000u);
        *eax = (uint32_t)t;
        *edx = (uint32_t)(t >> 32);
        return;
    }
#if defined(_WIN32)
    if (timespec_get(&ts, TIME_UTC) == TIME_UTC) {
#else
    /* Monotonic, as a TSC is; timespec_get is missing from older C
     * libraries (Android's before API 29). */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
#endif
        t = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    }
    *eax = (uint32_t)t;
    *edx = (uint32_t)(t >> 32);
}
