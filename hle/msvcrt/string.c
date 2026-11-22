/* SPDX-License-Identifier: MIT */
/*
 * MSVCP60: std::basic_string<char> as Visual C++ 6's library makes it: { allocator (one byte,
 * padded), _Ptr, _Len, _Res }, the characters shared between copies with a reference count in
 * the byte before them (0: one owner, 255: frozen). Most of the class is inline code in the
 * program; these are the members it calls. The behaviour follows the published header, in our
 * own code.
 */
#include "msvcrt.h"

#include <string.h>

#include "rc/log.h"
#include "rc/mem.h"
#include "rc/vm.h"

#define FROZEN 255u
#define MIN_SIZE 31u
#define NPOS 0xffffffffu
#define MAX_SIZE (NPOS - 2u)

/* The object's fields. */
#define PTR(s) rc_ld32((s) + 4)
#define LEN(s) rc_ld32((s) + 8)
#define RES(s) rc_ld32((s) + 12)

static uint32_t refcnt(uint32_t ptr) { return rc_ld8(ptr - 1); }

static void set(uint32_t s, uint32_t ptr, uint32_t len, uint32_t res)
{
    rc_st32(s + 4, ptr);
    rc_st32(s + 8, len);
    rc_st32(s + 12, res);
}

static void eos(uint32_t s, uint32_t n)
{
    rc_st32(s + 8, n);
    rc_st8(PTR(s) + n, 0);
}

/* _Tidy(built): drop the characters (or one reference to them). */
static void tidy(uint32_t s, bool built)
{
    uint32_t p = PTR(s);
    if (built && p) {
        uint32_t r = refcnt(p);
        if (r == 0 || r == FROZEN) {
            rc_crt_free(p - 1);
        } else {
            rc_st8(p - 1, (uint8_t)(r - 1));
        }
    }
    set(s, 0, 0, 0);
}

/* _Copy(n): a buffer of its own for at least n characters, the old ones kept. */
static void copy_to_new(uint32_t s, uint32_t n)
{
    uint32_t ns = n | MIN_SIZE;
    if (ns > MAX_SIZE) {
        ns = n;
    }
    uint32_t buf = rc_crt_malloc(ns + 2);
    uint32_t len = LEN(s);
    if (len) {
        memmove(rc_ptr(buf + 1), rc_ptr(PTR(s)), len > ns ? ns : len);
    }
    tidy(s, true);
    set(s, buf + 1, 0, ns);
    rc_st8(buf, 0);
    eos(s, len > ns ? ns : len);
}

/* _Grow(n, trim): room for n characters; false when n is 0 (nothing to copy then). */
static bool grow(uint32_t s, uint32_t n, bool trim)
{
    if (n > MAX_SIZE) {
        rc_fatal("std::string too long (std::length_error)");
    }
    uint32_t p = PTR(s);
    if (p && refcnt(p) != 0 && refcnt(p) != FROZEN) {
        if (n == 0) {
            rc_st8(p - 1, (uint8_t)(refcnt(p) - 1));
            tidy(s, false);
            return false;
        }
        copy_to_new(s, n);
        return true;
    }
    if (n == 0) {
        if (trim) {
            tidy(s, true);
        } else if (p) {
            eos(s, 0);
        }
        return false;
    }
    if (trim && (MIN_SIZE < RES(s) || RES(s) < n)) {
        tidy(s, true);
        copy_to_new(s, n);
    } else if (!trim && RES(s) < n) {
        copy_to_new(s, n);
    }
    return true;
}

static void assign_chars(uint32_t s, uint32_t src, uint32_t n)
{
    if (grow(s, n, true)) {
        memmove(rc_ptr(PTR(s)), rc_ptr(src), n);
        eos(s, n);
    }
}

/* _Split(): characters of its own before they are changed. */
static void split(uint32_t s)
{
    uint32_t p = PTR(s);
    if (p && refcnt(p) != 0 && refcnt(p) != FROZEN) {
        uint32_t len = LEN(s);
        uint32_t tmp = rc_crt_malloc(len + 1);
        memmove(rc_ptr(tmp), rc_ptr(p), len);
        tidy(s, true);
        assign_chars(s, tmp, len);
        rc_crt_free(tmp);
    }
}

static void erase(uint32_t s, uint32_t pos, uint32_t m)
{
    if (LEN(s) < pos) {
        rc_fatal("std::string: position out of range (std::out_of_range)");
    }
    split(s);
    uint32_t len = LEN(s);
    if (len - pos < m) {
        m = len - pos;
    }
    if (m) {
        uint32_t p = PTR(s);
        memmove(rc_ptr(p + pos), rc_ptr(p + pos + m), len - pos - m);
        uint32_t n = len - m;
        if (grow(s, n, false)) {
            eos(s, n);
        }
    }
}

/* The null string's character (_Nullstr's static), and npos. */
static uint32_t nullchar, npos_var;

static uint32_t var_nullchar(void)
{
    if (!nullchar) {
        nullchar = rc_heap_alloc(4, true);
    }
    return nullchar;
}

static uint32_t var_npos(void)
{
    if (!npos_var) {
        npos_var = rc_heap_alloc(4, true);
        rc_st32(npos_var, NPOS);
    }
    return npos_var;
}

/* All thiscall: this in ECX, the callee pops its arguments. */

/* basic_string(const char *, const allocator &) */
static void k_ctor_str(rc_cpu *c)
{
    uint32_t s = c->r[RC_ECX], src = RC_ARG(c, 0);
    rc_st32(s, 0);
    set(s, 0, 0, 0);
    assign_chars(s, src, (uint32_t)strlen(rc_str(src)));
    RC_RETURN(c, s, 2);
}

static void k_dtor(rc_cpu *c)
{
    tidy(c->r[RC_ECX], true);
    RC_RETURN(c, 0, 0);
}

/* append(const char *, size_t) */
static void k_append(rc_cpu *c)
{
    uint32_t s = c->r[RC_ECX], src = RC_ARG(c, 0), m = RC_ARG(c, 1);
    if (NPOS - LEN(s) <= m) {
        rc_fatal("std::string too long (std::length_error)");
    }
    uint32_t n = LEN(s) + m;
    if (m && grow(s, n, false)) {
        memmove(rc_ptr(PTR(s) + LEN(s)), rc_ptr(src), m);
        eos(s, n);
    }
    RC_RETURN(c, s, 2);
}

/* assign(const char *, size_t) */
static void k_assign_str(rc_cpu *c)
{
    uint32_t s = c->r[RC_ECX];
    assign_chars(s, RC_ARG(c, 0), RC_ARG(c, 1));
    RC_RETURN(c, s, 2);
}

/* assign(const basic_string &, size_t pos, size_t n): shares the characters when it takes them
 * all. */
static void k_assign_sub(rc_cpu *c)
{
    uint32_t s = c->r[RC_ECX], x = RC_ARG(c, 0), pos = RC_ARG(c, 1), m = RC_ARG(c, 2);
    uint32_t xlen = LEN(x);
    if (xlen < pos) {
        rc_fatal("std::string: position out of range (std::out_of_range)");
    }
    uint32_t n = xlen - pos;
    if (m < n) {
        n = m;
    }
    if (s == x) {
        erase(s, pos + n, NPOS);
        erase(s, 0, pos);
    } else if (n && n == xlen && refcnt(PTR(x)) < FROZEN - 1) {
        tidy(s, true);
        uint32_t p = PTR(x);
        set(s, p, xlen, RES(x));
        rc_st8(p - 1, (uint8_t)(refcnt(p) + 1));
    } else if (grow(s, n, true)) {
        memmove(rc_ptr(PTR(s)), rc_ptr((PTR(x) ? PTR(x) : var_nullchar()) + pos), n);
        eos(s, n);
    }
    RC_RETURN(c, s, 3);
}

static void k_erase(rc_cpu *c)
{
    uint32_t s = c->r[RC_ECX];
    erase(s, RC_ARG(c, 0), RC_ARG(c, 1));
    RC_RETURN(c, s, 2);
}

static void k_copy(rc_cpu *c)
{
    copy_to_new(c->r[RC_ECX], RC_ARG(c, 0));
    RC_RETURN(c, 0, 1);
}

static void k_eos(rc_cpu *c)
{
    eos(c->r[RC_ECX], RC_ARG(c, 0));
    RC_RETURN(c, 0, 1);
}

static void k_grow(rc_cpu *c)
{
    bool r = grow(c->r[RC_ECX], RC_ARG(c, 0), (RC_ARG(c, 1) & 0xff) != 0);
    RC_RETURN(c, r ? 1 : 0, 2);
}

static void k_split(rc_cpu *c)
{
    split(c->r[RC_ECX]);
    RC_RETURN(c, 0, 0);
}

static void k_tidy(rc_cpu *c)
{
    tidy(c->r[RC_ECX], (RC_ARG(c, 0) & 0xff) != 0);
    RC_RETURN(c, 0, 1);
}

#define S "?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@"

static const rc_import_def defs[] = {
    {MSVCP60, "??0" S "QAE@PBDABV?$allocator@D@1@@Z", k_ctor_str},
    {MSVCP60, "??1" S "QAE@XZ", k_dtor},
    {MSVCP60, "?append@" S "QAEAAV12@PBDI@Z", k_append},
    {MSVCP60, "?assign@" S "QAEAAV12@PBDI@Z", k_assign_str},
    {MSVCP60, "?assign@" S "QAEAAV12@ABV12@II@Z", k_assign_sub},
    {MSVCP60, "?erase@" S "QAEAAV12@II@Z", k_erase},
    {MSVCP60, "?_Copy@" S "AAEXI@Z", k_copy},
    {MSVCP60, "?_Eos@" S "AAEXI@Z", k_eos},
    {MSVCP60, "?_Grow@" S "AAE_NI_N@Z", k_grow},
    {MSVCP60, "?_Split@" S "AAEXXZ", k_split},
    {MSVCP60, "?_Tidy@" S "AAEX_N@Z", k_tidy},
};

void rc_msvcp60_string_init(void)
{
    rc_register_imports(defs, sizeof defs / sizeof defs[0]);
    rc_register_data(MSVCP60, "?_C@?1??_Nullstr@" S "CAPBDXZ@4DB", var_nullchar);
    rc_register_data(MSVCP60, "?npos@" S "2IB", var_npos);
}
