/* SPDX-License-Identifier: MIT */
/*
 * MSVCRT: printf and scanf with the arguments on the guest stack, the way MSVC 6 formats
 * (three-digit exponents, %I64, %p as eight hex digits), and _ecvt/_fcvt.
 */
#include "msvcrt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc/mem.h"
#include "rc/vm.h"
#include "rc/x87.h"

typedef struct out {
    char *buf;
    uint32_t cap, len;
} out;

static void put(out *o, char ch)
{
    if (o->cap && o->len + 1 < o->cap) {
        o->buf[o->len] = ch;
    }
    o->len++;
}

static void puts_n(out *o, const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        put(o, s[i]);
    }
}

static void pad(out *o, char ch, int n)
{
    while (n-- > 0) {
        put(o, ch);
    }
}

/* A double as MSVC 6 prints it: its exponents have three digits. */
static int fmt_double(char *buf, size_t cap, char conv, int prec, const char *flags, double v)
{
    char spec[32];
    snprintf(spec, sizeof spec, "%%%s.%d%c", flags, prec, conv);
    int n = snprintf(buf, cap, spec, v);
    if (n < 0 || (size_t)n >= cap) {
        return n;
    }
    if (isfinite(v) && (conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G')) {
        char *e = strpbrk(buf, "eE");
        if (e && (e[1] == '+' || e[1] == '-') && strlen(e + 2) == 2 && (size_t)n + 1 < cap) {
            memmove(e + 3, e + 2, strlen(e + 2) + 1);
            e[2] = '0';
            n++;
        }
    }
    if (!isfinite(v)) {
        /* MSVC: 1.#INF00, -1.#IND00, 1.#QNAN0 */
        const char *t = isnan(v) ? (signbit(v) ? "-1.#IND00" : "1.#QNAN0") : (v < 0 ? "-1.#INF00" : "1.#INF00");
        n = snprintf(buf, cap, "%s", t);
    }
    return n;
}

uint32_t rc_crt_format(char *buf, uint32_t cap, const char *fmt, uint32_t ap)
{
    out o = {buf, cap, 0};
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            put(&o, *p);
            continue;
        }
        p++;
        char flags[8] = "";
        size_t nf = 0;
        while (*p && strchr("-+ #0", *p)) {
            if (nf < sizeof flags - 1 && !strchr(flags, *p)) {
                flags[nf++] = *p;
                flags[nf] = 0;
            }
            p++;
        }
        int width = -1, prec = -1;
        if (*p == '*') {
            width = (int32_t)rc_ld32(ap);
            ap += 4;
            if (width < 0) {
                if (nf < sizeof flags - 1 && !strchr(flags, '-')) {
                    flags[nf++] = '-';
                    flags[nf] = 0;
                }
                width = -width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                width = (width < 0 ? 0 : width * 10) + (*p++ - '0');
            }
        }
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                prec = (int32_t)rc_ld32(ap);
                ap += 4;
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    prec = prec * 10 + (*p++ - '0');
                }
            }
        }
        int is64 = 0;
        if (*p == 'h' || *p == 'l' || *p == 'L' || *p == 'w') {
            p++;
        } else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') {
            is64 = 1;
            p += 3;
        }
        bool left = strchr(flags, '-') != NULL, zero = strchr(flags, '0') != NULL && !left;
        char tmp[512];
        int n = 0;
        switch (*p) {
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            uint64_t v;
            if (is64) {
                v = rc_ld64(ap);
                ap += 8;
            } else {
                uint32_t w = rc_ld32(ap);
                ap += 4;
                v = (*p == 'd' || *p == 'i') ? (uint64_t)(int64_t)(int32_t)w : w;
            }
            char spec[40];
            const char *len = "ll";
            if (prec >= 0) {
                snprintf(spec, sizeof spec, "%%%s.%d%s%c", flags, prec, len, *p);
            } else {
                snprintf(spec, sizeof spec, "%%%s%s%c", flags, len, *p);
            }
            if (*p == 'd' || *p == 'i') {
                n = snprintf(tmp, sizeof tmp, spec, (long long)v);
            } else {
                n = snprintf(tmp, sizeof tmp, spec, (unsigned long long)v);
            }
            break;
        }
        case 'p':
            n = snprintf(tmp, sizeof tmp, "%08X", rc_ld32(ap));
            ap += 4;
            break;
        case 'c':
            tmp[0] = (char)rc_ld32(ap);
            ap += 4;
            n = 1;
            break;
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G': {
            double v = rc_crt_f64(ap);
            ap += 8;
            char fl[8];
            size_t k = 0;
            for (const char *f = flags; *f; f++) {
                if (*f != '-' && *f != '0') {
                    fl[k++] = *f;
                }
            }
            fl[k] = 0;
            n = fmt_double(tmp, sizeof tmp, *p, prec < 0 ? 6 : prec, fl, v);
            if (n < 0 || n >= (int)sizeof tmp) {
                n = (int)strlen(tmp);
            }
            break;
        }
        case 's': {
            uint32_t s = rc_ld32(ap);
            ap += 4;
            const char *str = s ? rc_str(s) : "(null)";
            uint32_t len = (uint32_t)strlen(str);
            if (prec >= 0 && (uint32_t)prec < len) {
                len = (uint32_t)prec;
            }
            if (!left) {
                pad(&o, ' ', width - (int)len);
            }
            puts_n(&o, str, len);
            if (left) {
                pad(&o, ' ', width - (int)len);
            }
            continue;
        }
        case 'n':
            rc_st32(rc_ld32(ap), o.len);
            ap += 4;
            continue;
        case '%':
            put(&o, '%');
            continue;
        case 0:
            p--;
            continue;
        default:
            put(&o, *p);
            continue;
        }
        if (n < 0) {
            n = 0;
        }
        if (n > (int)sizeof tmp - 1) {
            n = (int)sizeof tmp - 1;
        }
        int padding = width - n;
        if (!left && !zero) {
            pad(&o, ' ', padding);
        }
        if (zero && padding > 0) {
            /* zeros after the sign or 0x */
            int lead = (tmp[0] == '-' || tmp[0] == '+' || tmp[0] == ' ') ? 1 : 0;
            if (tmp[lead] == '0' && (tmp[lead + 1] == 'x' || tmp[lead + 1] == 'X')) {
                lead += 2;
            }
            puts_n(&o, tmp, (uint32_t)lead);
            pad(&o, '0', padding);
            puts_n(&o, tmp + lead, (uint32_t)(n - lead));
        } else {
            puts_n(&o, tmp, (uint32_t)n);
        }
        if (left) {
            pad(&o, ' ', padding);
        }
    }
    if (cap) {
        o.buf[o.len < cap ? o.len : cap - 1] = 0;
    }
    return o.len;
}

/* ---- scanf ---- */

int rc_crt_scan(const char *in, const char *fmt, uint32_t ap)
{
    const char *s = in;
    int assigned = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n') {
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
                s++;
            }
            continue;
        }
        if (*p != '%') {
            if (*s != *p) {
                return assigned;
            }
            s++;
            continue;
        }
        p++;
        bool skip = false;
        if (*p == '*') {
            skip = true;
            p++;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p++ - '0');
        }
        int size = 4; /* h: 2, l: 4, L/I64: 8 (doubles: l → 8) */
        bool longer = false;
        if (*p == 'h') {
            size = 2;
            p++;
        } else if (*p == 'l') {
            longer = true;
            p++;
        } else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') {
            size = 8;
            p += 3;
        } else if (*p == 'L') {
            longer = true;
            p++;
        }
        if (*p == '%') {
            if (*s != '%') {
                return assigned;
            }
            s++;
            continue;
        }
        if (*p != 'c' && *p != '[' && *p != 'n') {
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
                s++;
            }
        }
        char field[256];
        int max = width ? width : (int)sizeof field - 1;
        if (max > (int)sizeof field - 1) {
            max = (int)sizeof field - 1;
        }
        switch (*p) {
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            int base = *p == 'x' || *p == 'X' ? 16 : *p == 'o' ? 8 : *p == 'i' ? 0 : 10;
            int k = 0;
            while (k < max && s[k] && (strchr("+-", s[k]) ? k == 0 : 1) &&
                   (strchr("+-0123456789abcdefABCDEFxX", s[k]) != NULL)) {
                k++;
            }
            memcpy(field, s, (size_t)k);
            field[k] = 0;
            char *end;
            long long v = strtoll(field, &end, base);
            if (end == field) {
                return assigned ? assigned : (*s ? 0 : -1);
            }
            s += end - field;
            if (!skip) {
                uint32_t dst = rc_ld32(ap);
                ap += 4;
                if (size == 2) {
                    rc_st16(dst, (uint16_t)v);
                } else if (size == 8) {
                    rc_st32(dst, (uint32_t)v);
                    rc_st32(dst + 4, (uint32_t)((unsigned long long)v >> 32));
                } else {
                    rc_st32(dst, (uint32_t)v);
                }
                assigned++;
            }
            break;
        }
        case 'e':
        case 'f':
        case 'g':
        case 'E':
        case 'G': {
            int k = 0;
            while (k < max && s[k] && strchr("+-0123456789.eE", s[k])) {
                k++;
            }
            memcpy(field, s, (size_t)k);
            field[k] = 0;
            char *end;
            double v = strtod(field, &end);
            if (end == field) {
                return assigned ? assigned : (*s ? 0 : -1);
            }
            s += end - field;
            if (!skip) {
                uint32_t dst = rc_ld32(ap);
                ap += 4;
                if (longer) {
                    uint64_t b = rc_f64bits(v);
                    rc_st32(dst, (uint32_t)b);
                    rc_st32(dst + 4, (uint32_t)(b >> 32));
                } else {
                    rc_st32(dst, rc_f32bits(v));
                }
                assigned++;
            }
            break;
        }
        case 's': {
            if (!*s) {
                return assigned ? assigned : -1;
            }
            int k = 0;
            while (k < (width ? width : 0x7fffffff) && s[k] && s[k] != ' ' && s[k] != '\t' && s[k] != '\n' &&
                   s[k] != '\r') {
                k++;
            }
            if (!skip) {
                uint32_t dst = rc_ld32(ap);
                ap += 4;
                memcpy(rc_ptr(dst), s, (size_t)k);
                rc_st8(dst + (uint32_t)k, 0);
                assigned++;
            }
            s += k;
            break;
        }
        case 'c': {
            int k = width ? width : 1;
            if ((int)strlen(s) < k) {
                return assigned ? assigned : -1;
            }
            if (!skip) {
                uint32_t dst = rc_ld32(ap);
                ap += 4;
                memcpy(rc_ptr(dst), s, (size_t)k);
                assigned++;
            }
            s += k;
            break;
        }
        case '[': {
            p++;
            bool neg = false;
            if (*p == '^') {
                neg = true;
                p++;
            }
            char set[256] = {0};
            if (*p == ']') {
                set[(unsigned char)']'] = 1;
                p++;
            }
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    for (int ch = (unsigned char)p[0]; ch <= (unsigned char)p[2]; ch++) {
                        set[ch] = 1;
                    }
                    p += 3;
                } else {
                    set[(unsigned char)*p++] = 1;
                }
            }
            int k = 0;
            while (k < (width ? width : 0x7fffffff) && s[k] && (set[(unsigned char)s[k]] != 0) != neg) {
                k++;
            }
            if (!k) {
                return assigned;
            }
            if (!skip) {
                uint32_t dst = rc_ld32(ap);
                ap += 4;
                memcpy(rc_ptr(dst), s, (size_t)k);
                rc_st8(dst + (uint32_t)k, 0);
                assigned++;
            }
            s += k;
            break;
        }
        case 'n':
            if (!skip) {
                rc_st32(rc_ld32(ap), (uint32_t)(s - in));
                ap += 4;
            }
            break;
        default:
            return assigned;
        }
    }
    return assigned;
}

/* ---- the imports ---- */

/* sprintf(buffer, format, ...) */
static void k_sprintf(rc_cpu *c)
{
    uint32_t dst = RC_ARG(c, 0);
    const char *fmt = rc_str(RC_ARG(c, 1));
    uint32_t n = rc_crt_format(NULL, 0, fmt, c->r[RC_ESP] + 12);
    char *tmp = malloc(n + 1);
    rc_crt_format(tmp, n + 1, fmt, c->r[RC_ESP] + 12);
    memcpy(rc_ptr(dst), tmp, n + 1);
    free(tmp);
    RC_RETURN(c, n, 0);
}

/* _snprintf / _vsnprintf: -1 when the result does not fit (no NUL then, as MSVC). */
static uint32_t bounded(uint32_t dst, uint32_t cap, const char *fmt, uint32_t ap)
{
    uint32_t n = rc_crt_format(NULL, 0, fmt, ap);
    char *tmp = malloc(n + 1);
    rc_crt_format(tmp, n + 1, fmt, ap);
    uint32_t k = n < cap ? n + 1 : cap;
    memcpy(rc_ptr(dst), tmp, k);
    free(tmp);
    return n < cap ? n : 0xffffffffu;
}

static void k__snprintf(rc_cpu *c)
{
    RC_RETURN(c, bounded(RC_ARG(c, 0), RC_ARG(c, 1), rc_str(RC_ARG(c, 2)), c->r[RC_ESP] + 16), 0);
}

static void k__vsnprintf(rc_cpu *c)
{
    RC_RETURN(c, bounded(RC_ARG(c, 0), RC_ARG(c, 1), rc_str(RC_ARG(c, 2)), RC_ARG(c, 3)), 0);
}

static void k_sscanf(rc_cpu *c)
{
    RC_RETURN(c, (uint32_t)rc_crt_scan(rc_str(RC_ARG(c, 0)), rc_str(RC_ARG(c, 1)), c->r[RC_ESP] + 12), 0);
}

/* _ecvt(value, digits, &decimal point, &sign) and _fcvt(value, decimals, …): the digits in a
 * static buffer. */
static uint32_t cvt_buf;

static void cvt(rc_cpu *c, bool fixed)
{
    double v = rc_crt_f64(c->r[RC_ESP] + 4);
    int32_t nd = (int32_t)RC_ARG(c, 2);
    uint32_t pdec = RC_ARG(c, 3), psign = RC_ARG(c, 4);
    if (!cvt_buf) {
        cvt_buf = rc_heap_alloc(352, true);
    }
    rc_st32(psign, signbit(v) ? 1 : 0);
    v = fabs(v);
    char tmp[400];
    int dec = 0;
    if (v == 0) {
        int k = nd < 0 ? 0 : nd > 340 ? 340 : nd;
        memset(tmp, '0', (size_t)k);
        tmp[k] = 0;
        dec = fixed ? 0 : 1;
    } else if (fixed) {
        int k = nd < 0 ? 0 : nd > 300 ? 300 : nd;
        snprintf(tmp, sizeof tmp, "%.*f", k, v);
        char *dot = strchr(tmp, '.');
        if (dot) {
            dec = (int)(dot - tmp);
            memmove(dot, dot + 1, strlen(dot));
        } else {
            dec = (int)strlen(tmp);
        }
        /* leading zeros move the decimal point */
        char *q = tmp;
        while (*q == '0' && q[1]) {
            q++;
            dec--;
        }
        memmove(tmp, q, strlen(q) + 1);
    } else {
        int k = nd < 1 ? 1 : nd > 340 ? 340 : nd;
        snprintf(tmp, sizeof tmp, "%.*e", k - 1, v);
        char *e = strchr(tmp, 'e');
        dec = e ? atoi(e + 1) + 1 : 1;
        if (e) {
            *e = 0;
        }
        char *dot = strchr(tmp, '.');
        if (dot) {
            memmove(dot, dot + 1, strlen(dot));
        }
    }
    rc_st32(pdec, (uint32_t)dec);
    rc_put_str(cvt_buf, 350, tmp);
    RC_RETURN(c, cvt_buf, 0);
}

static void k__ecvt(rc_cpu *c) { cvt(c, false); }
static void k__fcvt(rc_cpu *c) { cvt(c, true); }

const rc_import_def rc_msvcrt_format[] = {
    {MSVCRT, "sprintf", k_sprintf},
    {MSVCRT, "_snprintf", k__snprintf},
    {MSVCRT, "_vsnprintf", k__vsnprintf},
    {MSVCRT, "sscanf", k_sscanf},
    {MSVCRT, "_ecvt", k__ecvt},
    {MSVCRT, "_fcvt", k__fcvt},
};
const uint32_t rc_msvcrt_format_count = sizeof rc_msvcrt_format / sizeof rc_msvcrt_format[0];
