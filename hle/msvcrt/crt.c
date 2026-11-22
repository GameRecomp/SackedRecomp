/* SPDX-License-Identifier: MIT */
/*
 * MSVCRT: start-up and exit, the heap, strings, numbers, threads.
 *
 * All of these are cdecl (the caller pops the arguments) unless noted. Doubles are passed on the
 * stack (8 bytes) and returned in ST(0); the _CI* helpers take theirs on the x87 stack.
 */
#include "msvcrt.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc/log.h"
#include "rc/mem.h"
#include "rc/process.h"
#include "rc/thread.h"
#include "rc/vm.h"
#include "rc/x87.h"

#define K32 "KERNEL32.dll"

/* ---- calling guest code ---- */

uint32_t rc_crt_call(rc_cpu *c, uint32_t fn, uint32_t n, const uint32_t *args)
{
    uint32_t esp = c->r[RC_ESP];
    uint32_t r = rc_call_guest(c, fn, n, args);
    if (!c->unwind) {
        c->r[RC_ESP] = esp;
    }
    return r;
}

uint32_t rc_crt_call_this(rc_cpu *c, uint32_t fn, uint32_t self, uint32_t n, const uint32_t *args)
{
    c->r[RC_ECX] = self;
    return rc_crt_call(c, fn, n, args);
}

uint32_t rc_crt_call_funclet(rc_cpu *c, uint32_t fn, uint32_t ebp)
{
    uint32_t saved[4] = {c->r[RC_EBX], c->r[RC_ESI], c->r[RC_EDI], c->r[RC_EBP]};
    c->r[RC_EBP] = ebp;
    uint32_t r = rc_crt_call(c, fn, 0, NULL);
    if (!c->unwind) {
        c->r[RC_EBX] = saved[0];
        c->r[RC_ESI] = saved[1];
        c->r[RC_EDI] = saved[2];
        c->r[RC_EBP] = saved[3];
    }
    return r;
}

double rc_crt_f64(uint32_t a) { return rc_f64(rc_ld64(a)); }

/* Arguments: a double at argument slot i (two slots). */
static double darg(rc_cpu *c, int i) { return rc_crt_f64(c->r[RC_ESP] + 4u + 4u * (uint32_t)i); }

/* Return a double in ST(0) (cdecl). */
#define RETURN_DOUBLE(c, v)                                                                        \
    do {                                                                                           \
        rc_fpush((c), (v));                                                                        \
        RC_RETURN((c), 0, 0);                                                                      \
    } while (0)

/* ---- start-up ---- */

static uint32_t acmdln, fmode, commode, adjust_fdiv, errno_var;

/* _acmdln: the command line (char *). The imports are bound before the process has its command
 * line, so the variable is filled when the start-up code first calls in. */
static uint32_t var_acmdln(void)
{
    if (!acmdln) {
        acmdln = rc_heap_alloc(4, true);
    }
    rc_st32(acmdln, rc_proc.cmdline);
    return acmdln;
}

static void startup_vars(void)
{
    if (acmdln) {
        rc_st32(acmdln, rc_proc.cmdline);
    }
}

static uint32_t var_adjust_fdiv(void)
{
    if (!adjust_fdiv) {
        adjust_fdiv = rc_heap_alloc(4, true); /* no Pentium FDIV bug here */
    }
    return adjust_fdiv;
}

static uint32_t int_var(uint32_t *v)
{
    if (!*v) {
        *v = rc_heap_alloc(4, true);
    }
    return *v;
}

static void k___set_app_type(rc_cpu *c)
{
    startup_vars();
    RC_RETURN(c, 0, 0);
}
static void k___setusermatherr(rc_cpu *c) { RC_RETURN(c, 0, 0); }
static void k___p__fmode(rc_cpu *c)
{
    startup_vars();
    RC_RETURN(c, int_var(&fmode), 0);
}
static void k___p__commode(rc_cpu *c) { RC_RETURN(c, int_var(&commode), 0); }
static void k__errno(rc_cpu *c) { RC_RETURN(c, int_var(&errno_var), 0); }

/* The command line split as the C runtime splits it: blanks separate, quotes group,
 * backslashes before a quote escape. */
static uint32_t split_args(const char *s, char **out, uint32_t max)
{
    uint32_t n = 0;
    static char buf[4096];
    char *w = buf, *end = buf + sizeof buf - 1;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (!*s) {
            break;
        }
        out[n++] = w;
        int quoted = 0;
        while (*s && (quoted || (*s != ' ' && *s != '\t')) && w < end) {
            if (*s == '"') {
                quoted = !quoted;
                s++;
            } else if (*s == '\\' && s[1] == '"') {
                *w++ = '"';
                s += 2;
            } else {
                *w++ = *s++;
            }
        }
        if (w < end) {
            *w++ = 0;
        }
    }
    return n;
}

/* __getmainargs(&argc, &argv, &envp, wildcards, &startinfo) */
static void k___getmainargs(rc_cpu *c)
{
    startup_vars();
    char *args[64];
    uint32_t n = split_args(rc_str(rc_proc.cmdline), args, 63);
    uint32_t argv = rc_heap_alloc(4 * (n + 1), true);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t len = (uint32_t)strlen(args[i]) + 1;
        uint32_t s = rc_heap_alloc(len, false);
        memcpy(rc_ptr(s), args[i], len);
        rc_st32(argv + 4 * i, s);
    }
    uint32_t envp = rc_heap_alloc(4, true);
    rc_st32(RC_ARG(c, 0), n);
    rc_st32(RC_ARG(c, 1), argv);
    rc_st32(RC_ARG(c, 2), envp);
    RC_RETURN(c, 0, 0);
}

/* _initterm(begin, end): the initialisers (or terminators) between, in order. */
static void k__initterm(rc_cpu *c)
{
    uint32_t begin = RC_ARG(c, 0), end = RC_ARG(c, 1);
    for (uint32_t p = begin; p < end; p += 4) {
        uint32_t fn = rc_ld32(p);
        if (fn) {
            rc_crt_call(c, fn, 0, NULL);
        }
    }
    RC_RETURN(c, 0, 0);
}

/* The x87 control word as _controlfp shows it (the bits are named differently). */
static uint32_t control_word = 0x0009001fu; /* _PC_53 | _RC_NEAR | all exceptions masked */

static void k__controlfp(rc_cpu *c)
{
    uint32_t value = RC_ARG(c, 0), mask = RC_ARG(c, 1);
    control_word = (control_word & ~mask) | (value & mask);
    RC_RETURN(c, control_word, 0);
}

/* ---- exit ---- */

#define ONEXIT_MAX 64
static uint32_t onexit_fns[ONEXIT_MAX];
static uint32_t nonexit;

static void run_onexit(rc_cpu *c)
{
    while (nonexit) {
        uint32_t fn = onexit_fns[--nonexit];
        rc_crt_call(c, fn, 0, NULL);
    }
}

static void k__onexit(rc_cpu *c)
{
    uint32_t fn = RC_ARG(c, 0);
    if (nonexit == ONEXIT_MAX) {
        RC_RETURN(c, 0, 0);
    }
    onexit_fns[nonexit++] = fn;
    RC_RETURN(c, fn, 0);
}

/* __dllonexit(fn, &begin, &end): a DLL's own table of terminators (it runs them itself). */
static void k___dllonexit(rc_cpu *c)
{
    uint32_t fn = RC_ARG(c, 0), pbegin = RC_ARG(c, 1), pend = RC_ARG(c, 2);
    uint32_t begin = rc_ld32(pbegin), end = rc_ld32(pend);
    uint32_t n = begin ? (end - begin) / 4 : 0;
    uint32_t table = rc_heap_alloc(4 * (n + 1), true);
    if (n) {
        memcpy(rc_ptr(table), rc_ptr(begin), 4 * n);
    }
    rc_st32(table + 4 * n, fn);
    rc_st32(pbegin, table);
    rc_st32(pend, table + 4 * (n + 1));
    RC_RETURN(c, fn, 0);
}

static void k_exit(rc_cpu *c)
{
    uint32_t code = RC_ARG(c, 0);
    run_onexit(c);
    rc_exit(code);
}

static void k__exit(rc_cpu *c) { rc_exit(RC_ARG(c, 0)); }

static void k_abort(rc_cpu *c)
{
    (void)c;
    rc_fatal("the program called abort()");
}

static void k__assert(rc_cpu *c)
{
    rc_fatal("assertion failed: %s (%s, line %u)", rc_str(RC_ARG(c, 0)), rc_str(RC_ARG(c, 1)), RC_ARG(c, 2));
}

/* ---- the heap ---- */

uint32_t rc_crt_malloc(uint32_t n) { return rc_heap_alloc(n ? n : 1, false); }
void rc_crt_free(uint32_t p)
{
    if (p) {
        rc_heap_free(p);
    }
}

static uint32_t new_handler, new_mode;

static void k_malloc(rc_cpu *c) { RC_RETURN(c, rc_crt_malloc(RC_ARG(c, 0)), 0); }

static void k_free(rc_cpu *c)
{
    rc_crt_free(RC_ARG(c, 0));
    RC_RETURN(c, 0, 0);
}

static void k_calloc(rc_cpu *c)
{
    uint64_t n = (uint64_t)RC_ARG(c, 0) * RC_ARG(c, 1);
    if (n > 0x7fffffffu) {
        RC_RETURN(c, 0, 0);
    }
    RC_RETURN(c, rc_heap_alloc(n ? (uint32_t)n : 1, true), 0);
}

static void k_realloc(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0), n = RC_ARG(c, 1);
    if (!p) {
        RC_RETURN(c, rc_crt_malloc(n), 0);
    }
    if (!n) {
        rc_crt_free(p);
        RC_RETURN(c, 0, 0);
    }
    RC_RETURN(c, rc_heap_realloc(p, n, false, false), 0);
}

/* operator new: on failure the new handler is asked to make room, as long as it says so. */
static void k_new(rc_cpu *c)
{
    uint32_t n = RC_ARG(c, 0);
    for (;;) {
        uint32_t p = rc_crt_malloc(n);
        if (p || !new_handler) {
            RC_RETURN(c, p, 0);
        }
        uint32_t a[1] = {n};
        if (!rc_crt_call(c, new_handler, 1, a)) {
            RC_RETURN(c, 0, 0);
        }
    }
}

static void k_set_new_handler(rc_cpu *c)
{
    uint32_t old = new_handler;
    new_handler = RC_ARG(c, 0);
    RC_RETURN(c, old, 0);
}

static void k_set_new_mode(rc_cpu *c)
{
    uint32_t old = new_mode;
    new_mode = RC_ARG(c, 0);
    RC_RETURN(c, old, 0);
}

static uint32_t guest_strdup(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t p = rc_crt_malloc(n);
    memcpy(rc_ptr(p), s, n);
    return p;
}

static void k__strdup(rc_cpu *c)
{
    const char *s = rc_str(RC_ARG(c, 0));
    RC_RETURN(c, s ? guest_strdup(s) : 0, 0);
}

/* ---- memory and strings ---- */

static void k_memmove(rc_cpu *c)
{
    uint32_t d = RC_ARG(c, 0), s = RC_ARG(c, 1), n = RC_ARG(c, 2);
    if (n) {
        memmove(rc_ptr(d), rc_ptr(s), n);
    }
    RC_RETURN(c, d, 0);
}

static void k_memchr(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0), ch = RC_ARG(c, 1) & 0xff, n = RC_ARG(c, 2);
    for (uint32_t i = 0; i < n; i++) {
        if (rc_ld8(p + i) == ch) {
            RC_RETURN(c, p + i, 0);
        }
    }
    RC_RETURN(c, 0, 0);
}

static void k_strchr(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0), ch = RC_ARG(c, 1) & 0xff;
    for (;; p++) {
        uint32_t b = rc_ld8(p);
        if (b == ch) {
            RC_RETURN(c, p, 0);
        }
        if (!b) {
            RC_RETURN(c, 0, 0);
        }
    }
}

static void k_strstr(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0);
    const char *hs = rc_str(h), *f = strstr(hs, rc_str(RC_ARG(c, 1)));
    RC_RETURN(c, f ? h + (uint32_t)(f - hs) : 0, 0);
}

static void k_strcspn(rc_cpu *c) { RC_RETURN(c, strcspn(rc_str(RC_ARG(c, 0)), rc_str(RC_ARG(c, 1))), 0); }

static void k_strncmp(rc_cpu *c)
{
    int r = strncmp(rc_str(RC_ARG(c, 0)), rc_str(RC_ARG(c, 1)), RC_ARG(c, 2));
    RC_RETURN(c, (uint32_t)(r < 0 ? -1 : r > 0), 0);
}

static int icmp(const char *a, const char *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y || !x) {
            return x - y;
        }
    }
    return 0;
}

static void k__stricmp(rc_cpu *c) { RC_RETURN(c, (uint32_t)icmp(rc_str(RC_ARG(c, 0)), rc_str(RC_ARG(c, 1)), ~0u), 0); }
static void k__strnicmp(rc_cpu *c)
{
    RC_RETURN(c, (uint32_t)icmp(rc_str(RC_ARG(c, 0)), rc_str(RC_ARG(c, 1)), RC_ARG(c, 2)), 0);
}

static void k_strncpy(rc_cpu *c)
{
    uint32_t d = RC_ARG(c, 0), s = RC_ARG(c, 1), n = RC_ARG(c, 2);
    uint32_t i = 0;
    for (; i < n; i++) {
        uint8_t b = rc_ld8(s + i);
        rc_st8(d + i, b);
        if (!b) {
            break;
        }
    }
    for (; i < n; i++) {
        rc_st8(d + i, 0);
    }
    RC_RETURN(c, d, 0);
}

static void k_strncat(rc_cpu *c)
{
    uint32_t d = RC_ARG(c, 0), s = RC_ARG(c, 1), n = RC_ARG(c, 2);
    uint32_t e = d + (uint32_t)strlen(rc_str(d));
    uint32_t i = 0;
    for (; i < n; i++) {
        uint8_t b = rc_ld8(s + i);
        if (!b) {
            break;
        }
        rc_st8(e + i, b);
    }
    rc_st8(e + i, 0);
    RC_RETURN(c, d, 0);
}

static void k_toupper(rc_cpu *c)
{
    uint32_t ch = RC_ARG(c, 0);
    RC_RETURN(c, ch >= 'a' && ch <= 'z' ? ch - 32 : ch, 0);
}

static void k_atoi(rc_cpu *c) { RC_RETURN(c, (uint32_t)atoi(rc_str(RC_ARG(c, 0))), 0); }

/* _splitpath(path, drive, dir, name, ext): any part may be NULL. */
static void k__splitpath(rc_cpu *c)
{
    const char *p = rc_str(RC_ARG(c, 0));
    uint32_t drive = RC_ARG(c, 1), dir = RC_ARG(c, 2), name = RC_ARG(c, 3), ext = RC_ARG(c, 4);
    char d[3] = "", buf[260];
    if (p[0] && p[1] == ':') {
        d[0] = p[0];
        d[1] = ':';
        p += 2;
    }
    const char *slash = NULL, *dot = NULL;
    for (const char *q = p; *q; q++) {
        if (*q == '\\' || *q == '/') {
            slash = q;
        }
    }
    const char *base = slash ? slash + 1 : p;
    for (const char *q = base; *q; q++) {
        if (*q == '.') {
            dot = q;
        }
    }
    if (drive) {
        rc_put_str(drive, 3, d);
    }
    if (dir) {
        size_t n = (size_t)(base - p) < sizeof buf - 1 ? (size_t)(base - p) : sizeof buf - 1;
        memcpy(buf, p, n);
        buf[n] = 0;
        rc_put_str(dir, 256, buf);
    }
    if (name) {
        size_t n = dot ? (size_t)(dot - base) : strlen(base);
        n = n < sizeof buf - 1 ? n : sizeof buf - 1;
        memcpy(buf, base, n);
        buf[n] = 0;
        rc_put_str(name, 256, buf);
    }
    if (ext) {
        rc_put_str(ext, 256, dot ? dot : "");
    }
    RC_RETURN(c, 0, 0);
}

/* ---- numbers ---- */

static uint32_t holdrand = 1;

static void k_rand(rc_cpu *c)
{
    holdrand = holdrand * 214013u + 2531011u;
    RC_RETURN(c, (holdrand >> 16) & 0x7fff, 0);
}

static void k_srand(rc_cpu *c)
{
    holdrand = RC_ARG(c, 0);
    RC_RETURN(c, 0, 0);
}

/* qsort(base, n, size, compare): the guest's compare (cdecl). A heap sort keeps the calls few
 * and needs no host recursion. */
static void k_qsort(rc_cpu *c)
{
    uint32_t base = RC_ARG(c, 0), n = RC_ARG(c, 1), size = RC_ARG(c, 2), cmp = RC_ARG(c, 3);
    if (n < 2 || !size) {
        RC_RETURN(c, 0, 0);
    }
    uint8_t *tmp = malloc(size);
#define AT(i) (base + (uint32_t)(i) * size)
#define LESS(i, j) ((int32_t)rc_crt_call(c, cmp, 2, (uint32_t[2]){AT(i), AT(j)}) < 0)
#define SWAP(i, j)                                                                                 \
    do {                                                                                           \
        memcpy(tmp, rc_ptr(AT(i)), size);                                                          \
        memmove(rc_ptr(AT(i)), rc_ptr(AT(j)), size);                                               \
        memcpy(rc_ptr(AT(j)), tmp, size);                                                          \
    } while (0)
    for (uint32_t start = n / 2; start-- > 0;) {
        for (uint32_t root = start;;) {
            uint32_t child = 2 * root + 1;
            if (child >= n) {
                break;
            }
            if (child + 1 < n && LESS(child, child + 1)) {
                child++;
            }
            if (!LESS(root, child)) {
                break;
            }
            SWAP(root, child);
            root = child;
        }
    }
    for (uint32_t end = n - 1; end > 0; end--) {
        SWAP(0, end);
        for (uint32_t root = 0;;) {
            uint32_t child = 2 * root + 1;
            if (child >= end) {
                break;
            }
            if (child + 1 < end && LESS(child, child + 1)) {
                child++;
            }
            if (!LESS(root, child)) {
                break;
            }
            SWAP(root, child);
            root = child;
        }
    }
#undef AT
#undef LESS
#undef SWAP
    free(tmp);
    RC_RETURN(c, 0, 0);
}

static void k_floor(rc_cpu *c) { RETURN_DOUBLE(c, floor(darg(c, 0))); }
static void k_ceil(rc_cpu *c) { RETURN_DOUBLE(c, ceil(darg(c, 0))); }
static void k__hypot(rc_cpu *c) { RETURN_DOUBLE(c, hypot(darg(c, 0), darg(c, 2))); }
static void k__copysign(rc_cpu *c) { RETURN_DOUBLE(c, copysign(darg(c, 0), darg(c, 2))); }
static void k_ldexp(rc_cpu *c) { RETURN_DOUBLE(c, ldexp(darg(c, 0), (int32_t)RC_ARG(c, 2))); }

static void k_frexp(rc_cpu *c)
{
    int e = 0;
    double m = frexp(darg(c, 0), &e);
    rc_st32(RC_ARG(c, 2), (uint32_t)e);
    RETURN_DOUBLE(c, m);
}

static void k__finite(rc_cpu *c) { RC_RETURN(c, isfinite(darg(c, 0)) ? 1 : 0, 0); }
static void k__isnan(rc_cpu *c) { RC_RETURN(c, isnan(darg(c, 0)) ? 1 : 0, 0); }

/* _fpclass: the MSVC class bits (_FPCLASS_*). */
static void k__fpclass(rc_cpu *c)
{
    double v = darg(c, 0);
    uint32_t r;
    switch (fpclassify(v)) {
    case FP_NAN: r = 0x0002; break; /* quiet NaN */
    case FP_INFINITE: r = signbit(v) ? 0x0004 : 0x0200; break;
    case FP_ZERO: r = signbit(v) ? 0x0020 : 0x0040; break;
    case FP_SUBNORMAL: r = signbit(v) ? 0x0010 : 0x0080; break;
    default: r = signbit(v) ? 0x0008 : 0x0100; break;
    }
    RC_RETURN(c, r, 0);
}

/* The _CI helpers: arguments on the x87 stack, the result in ST(0). */
static void k__CIpow(rc_cpu *c)
{
    double y = RC_ST(c, 0), x = RC_ST(c, 1);
    rc_fpop(c);
    RC_ST(c, 0) = pow(x, y);
    RC_RETURN(c, 0, 0);
}

static void k__CIacos(rc_cpu *c)
{
    RC_ST(c, 0) = acos(RC_ST(c, 0));
    RC_RETURN(c, 0, 0);
}

static void k__CIsinh(rc_cpu *c)
{
    RC_ST(c, 0) = sinh(RC_ST(c, 0));
    RC_RETURN(c, 0, 0);
}

static void k__CIcosh(rc_cpu *c)
{
    RC_ST(c, 0) = cosh(RC_ST(c, 0));
    RC_RETURN(c, 0, 0);
}

/* _ftol: ST(0) truncated to a 64-bit integer in EDX:EAX, popped. */
static void k__ftol(rc_cpu *c)
{
    double v = RC_ST(c, 0);
    rc_fpop(c);
    int64_t r = (v >= -9.2233720368547758e18 && v < 9.2233720368547758e18) ? (int64_t)v : INT64_MIN;
    c->r[RC_EDX] = (uint32_t)((uint64_t)r >> 32);
    RC_RETURN(c, (uint32_t)r, 0);
}

/* ---- locale ---- */

static uint32_t locale_name;

static void k_setlocale(rc_cpu *c)
{
    const char *want = rc_str(RC_ARG(c, 1));
    if (!locale_name) {
        locale_name = rc_proc_str("C");
    }
    /* Only the C locale is there. */
    if (want && *want && strcmp(want, "C") != 0) {
        RC_RETURN(c, 0, 0);
    }
    RC_RETURN(c, locale_name, 0);
}

/* ---- threads ---- */

/* _beginthreadex(security, stack, start, arg, flags, &id): a Win32 thread (start is stdcall). */
static void k__beginthreadex(rc_cpu *c)
{
    uint32_t a[6] = {RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3), RC_ARG(c, 4), RC_ARG(c, 5)};
    uint32_t h = rc_crt_call(c, rc_import_address(K32, "CreateThread"), 6, a);
    RC_RETURN(c, h, 0);
}

static void k__endthreadex(rc_cpu *c)
{
    uint32_t a[1] = {RC_ARG(c, 0)};
    rc_crt_call(c, rc_import_address(K32, "ExitThread"), 1, a);
    if (c->unwind) {
        return; /* (in a browser: the thread unwinds to its start) */
    }
    RC_RETURN(c, 0, 0);
}

const rc_import_def rc_msvcrt_crt[] = {
    {MSVCRT, "__set_app_type", k___set_app_type},
    {MSVCRT, "__setusermatherr", k___setusermatherr},
    {MSVCRT, "__p__fmode", k___p__fmode},
    {MSVCRT, "__p__commode", k___p__commode},
    {MSVCRT, "__getmainargs", k___getmainargs},
    {MSVCRT, "_initterm", k__initterm},
    {MSVCRT, "_controlfp", k__controlfp},
    {MSVCRT, "_onexit", k__onexit},
    {MSVCRT, "__dllonexit", k___dllonexit},
    {MSVCRT, "exit", k_exit},
    {MSVCRT, "_exit", k__exit},
    {MSVCRT, "abort", k_abort},
    {MSVCRT, "_assert", k__assert},
    {MSVCRT, "_errno", k__errno},
    {MSVCRT, "malloc", k_malloc},
    {MSVCRT, "free", k_free},
    {MSVCRT, "calloc", k_calloc},
    {MSVCRT, "realloc", k_realloc},
    {MSVCRT, "??2@YAPAXI@Z", k_new},
    {MSVCRT, "?_set_new_handler@@YAP6AHI@ZP6AHI@Z@Z", k_set_new_handler},
    {MSVCRT, "?_set_new_mode@@YAHH@Z", k_set_new_mode},
    {MSVCRT, "_strdup", k__strdup},
    {MSVCRT, "memmove", k_memmove},
    {MSVCRT, "memcpy", k_memmove},
    {MSVCRT, "memchr", k_memchr},
    {MSVCRT, "strchr", k_strchr},
    {MSVCRT, "strstr", k_strstr},
    {MSVCRT, "strcspn", k_strcspn},
    {MSVCRT, "strncmp", k_strncmp},
    {MSVCRT, "_stricmp", k__stricmp},
    {MSVCRT, "_strnicmp", k__strnicmp},
    {MSVCRT, "strncpy", k_strncpy},
    {MSVCRT, "strncat", k_strncat},
    {MSVCRT, "toupper", k_toupper},
    {MSVCRT, "atoi", k_atoi},
    {MSVCRT, "_splitpath", k__splitpath},
    {MSVCRT, "rand", k_rand},
    {MSVCRT, "srand", k_srand},
    {MSVCRT, "qsort", k_qsort},
    {MSVCRT, "floor", k_floor},
    {MSVCRT, "ceil", k_ceil},
    {MSVCRT, "_hypot", k__hypot},
    {MSVCRT, "_copysign", k__copysign},
    {MSVCRT, "ldexp", k_ldexp},
    {MSVCRT, "frexp", k_frexp},
    {MSVCRT, "_finite", k__finite},
    {MSVCRT, "_isnan", k__isnan},
    {MSVCRT, "_fpclass", k__fpclass},
    {MSVCRT, "_CIpow", k__CIpow},
    {MSVCRT, "_CIacos", k__CIacos},
    {MSVCRT, "_CIsinh", k__CIsinh},
    {MSVCRT, "_CIcosh", k__CIcosh},
    {MSVCRT, "_ftol", k__ftol},
    {MSVCRT, "setlocale", k_setlocale},
    {MSVCRT, "_beginthreadex", k__beginthreadex},
    {MSVCRT, "_endthreadex", k__endthreadex},
};
const uint32_t rc_msvcrt_crt_count = sizeof rc_msvcrt_crt / sizeof rc_msvcrt_crt[0];

void rc_msvcrt_init(void)
{
    rc_register_imports(rc_msvcrt_crt, rc_msvcrt_crt_count);
    rc_register_data(MSVCRT, "_acmdln", var_acmdln);
    rc_register_data(MSVCRT, "_adjust_fdiv", var_adjust_fdiv);
    rc_msvcrt_stdio_init();
    rc_msvcrt_eh_init();
    rc_register_imports(rc_msvcp60_defs, rc_msvcp60_count);
    rc_msvcp60_string_init();
}
