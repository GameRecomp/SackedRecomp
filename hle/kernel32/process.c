/* SPDX-License-Identifier: MIT */
/*
 * kernel32: the process, its threads (one), modules, the environment, time
 * and synchronisation. The program is single-threaded, so critical
 * sections and interlocked operations need no host synchronisation.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "k32.h"
#include "rc/log.h"
#include "rc/thread.h"
#include "rc/vm.h"

/* Pseudo module handles for DLLs we answer for: high, aligned, unmapped. */
#define MODULE_BASE 0x7a000000u
static const char *const known_modules[] = {
    "KERNEL32", "USER32", "GDI32", "DDRAW", "MSS32", "BINKW32", "ADVAPI32", "WINMM", "OLE32", "VERSION",
    "DINPUT", "DSOUND", "SHELL32", "WSOCK32", "WS2_32", "DPLAYX", "DPNET", "COMCTL32", "COMDLG32", "WINSPOOL",
};
#define NMODULES (sizeof known_modules / sizeof known_modules[0])

static int module_index(const char *name)
{
    char base[64];
    const char *s = strrchr(name, '\\');
    snprintf(base, sizeof base, "%s", s ? s + 1 : name);
    char *dot = strrchr(base, '.');
    if (dot) {
        *dot = 0;
    }
    for (char *p = base; *p; p++) {
        if (*p >= 'a' && *p <= 'z') {
            *p = (char)(*p - 32);
        }
    }
    for (unsigned i = 0; i < NMODULES; i++) {
        if (strcmp(base, known_modules[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void k_GetVersion(rc_cpu *c)
{
    /* Windows XP SP3: 5.1, build 2600. */
    RC_RETURN(c, 0x0a280105u, 0);
}

static void k_GetVersionExA(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    uint32_t size = rc_ld32(p);
    if (size < 148) {
        rc_last_error = ERROR_INSUFFICIENT_BUFFER;
        RC_RETURN(c, 0, 1);
    }
    rc_st32(p + 4, 5);
    rc_st32(p + 8, 1);
    rc_st32(p + 12, 2600);
    rc_st32(p + 16, 2); /* VER_PLATFORM_WIN32_NT */
    memset(rc_ptr(p + 20), 0, 128);
    rc_put_str(p + 20, 128, "Service Pack 3");
    RC_RETURN(c, 1, 1);
}

static void k_GetCommandLineA(rc_cpu *c) { RC_RETURN(c, rc_proc.cmdline, 0); }

static void k_GetStartupInfoA(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    memset(rc_ptr(p), 0, 68);
    rc_st32(p, 68);
    RC_RETURN(c, 0, 1);
}

static void k_GetModuleHandleA(rc_cpu *c)
{
    uint32_t name = RC_ARG(c, 0);
    if (name == 0) {
        RC_RETURN(c, rc_proc.image_base, 1);
    }
    const rc_module *lm = rc_module_find(rc_str(name));
    if (lm) {
        RC_RETURN(c, lm->base, 1);
    }
    int i = module_index(rc_str(name));
    if (i >= 0) {
        RC_RETURN(c, MODULE_BASE + 0x100000u * (uint32_t)i, 1);
    }
    rc_last_error = 126; /* ERROR_MOD_NOT_FOUND */
    RC_RETURN(c, 0, 1);
}

static void k_LoadLibraryA(rc_cpu *c)
{
    const rc_module *lm = rc_module_find(rc_str(RC_ARG(c, 0)));
    if (lm) {
        RC_RETURN(c, lm->base, 1);
    }
    int i = module_index(rc_str(RC_ARG(c, 0)));
    if (i >= 0) {
        RC_RETURN(c, MODULE_BASE + 0x100000u * (uint32_t)i, 1);
    }
    rc_log("LoadLibraryA(\"%s\"): not provided", rc_str(RC_ARG(c, 0)));
    rc_last_error = 126;
    RC_RETURN(c, 0, 1);
}

static void k_FreeLibrary(rc_cpu *c) { RC_RETURN(c, 1, 1); }

static void k_GetProcAddress(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0), name = RC_ARG(c, 1);
    const rc_module *lm = rc_module_at(h);
    if (lm) {
        char ord[16];
        if (name < 0x10000) {
            snprintf(ord, sizeof ord, "#%u", name);
        }
        uint32_t a = rc_module_export(lm, name < 0x10000 ? ord : rc_str(name));
        if (!a) {
            rc_last_error = 127;
        }
        RC_RETURN(c, a, 2);
    }
    const char *dll = NULL;
    if (h >= MODULE_BASE && (h - MODULE_BASE) % 0x100000u == 0 && (h - MODULE_BASE) / 0x100000u < NMODULES) {
        dll = known_modules[(h - MODULE_BASE) / 0x100000u];
    }
    if (dll == NULL || name < 0x10000) {
        rc_last_error = 127; /* ERROR_PROC_NOT_FOUND */
        RC_RETURN(c, 0, 2);
    }
    /* The name must outlive the guest string. */
    const char *n = rc_str(rc_proc_str(rc_str(name)));
    RC_RETURN(c, rc_import_address(dll, n), 2);
}

static void k_GetModuleFileNameA(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0), buf = RC_ARG(c, 1), n = RC_ARG(c, 2);
    const rc_module *lm = h ? rc_module_at(h) : NULL;
    if (lm && h != rc_proc.image_base) {
        char path[300];
        snprintf(path, sizeof path, "%s\\%s", rc_cfg->guest_dir, lm->name);
        RC_RETURN(c, rc_put_str(buf, n, path), 3);
    }
    RC_RETURN(c, rc_put_str(buf, n, rc_str(rc_proc.module_path)), 3);
}

static void k_GetWindowsDirectoryA(rc_cpu *c) { RC_RETURN(c, rc_put_str(RC_ARG(c, 0), RC_ARG(c, 1), "C:\\WINDOWS"), 2); }

static void k_GetSystemDirectoryA(rc_cpu *c)
{
    RC_RETURN(c, rc_put_str(RC_ARG(c, 0), RC_ARG(c, 1), "C:\\WINDOWS\\system32"), 2);
}

static uint32_t error_mode;

static void k_SetErrorMode(rc_cpu *c)
{
    uint32_t old = error_mode;
    error_mode = RC_ARG(c, 0);
    RC_RETURN(c, old, 1);
}

static void k_GetCurrentProcessId(rc_cpu *c) { RC_RETURN(c, 1, 0); }

/* The performance counter runs at 1 MHz on the process clock (virtual in
 * scripted runs). */
static void k_QueryPerformanceFrequency(rc_cpu *c)
{
    rc_st32(RC_ARG(c, 0), 1000000u);
    rc_st32(RC_ARG(c, 0) + 4, 0);
    RC_RETURN(c, 1, 1);
}

static void k_QueryPerformanceCounter(rc_cpu *c)
{
    rc_thread_yield();
    rc_thread_poll();
    uint64_t us = rc_ticks_ms() * 1000u;
    rc_st32(RC_ARG(c, 0), (uint32_t)us);
    rc_st32(RC_ARG(c, 0) + 4, (uint32_t)(us >> 32));
    RC_RETURN(c, 1, 1);
}

static void k_ExitProcess(rc_cpu *c) { rc_exit(RC_ARG(c, 0)); }

static void k_TerminateProcess(rc_cpu *c)
{
    if (RC_ARG(c, 0) == 0xffffffffu) {
        rc_exit(RC_ARG(c, 1));
    }
    RC_RETURN(c, 0, 2);
}

static void k_FatalAppExitA(rc_cpu *c)
{
    rc_log("FatalAppExitA: %s", rc_str(RC_ARG(c, 1)));
    rc_exit(255);
}

static void k_GetCurrentProcess(rc_cpu *c) { RC_RETURN(c, 0xffffffffu, 0); }
static void k_GetCurrentThread(rc_cpu *c) { RC_RETURN(c, 0xfffffffeu, 0); }
static void k_GetCurrentThreadId(rc_cpu *c) { RC_RETURN(c, rc_thread_id(), 0); }
/* DisableThreadLibraryCalls(module): DllMain is never called for threads here anyway. */
static void k_DisableThreadLibraryCalls(rc_cpu *c) { RC_RETURN(c, 1, 1); }

static void k_CreateThread(rc_cpu *c)
{
    uint32_t stack = RC_ARG(c, 1), start = RC_ARG(c, 2), param = RC_ARG(c, 3);
    uint32_t flags = RC_ARG(c, 4), id_out = RC_ARG(c, 5);
    uint32_t id = 0;
    uint32_t h = rc_thread_create(start, param, stack, &id);
    if (h && (flags & 4)) { /* CREATE_SUSPENDED: until ResumeThread */
        rc_thread_suspend(h, 1);
    }
    if (!h) {
        rc_last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
    }
    if (h && id_out) {
        rc_st32(id_out, id);
    }
    RC_RETURN(c, h, 6);
}

static void k_ResumeThread(rc_cpu *c)
{
    int n = rc_thread_suspend(RC_ARG(c, 0), -1);
    RC_RETURN(c, (uint32_t)n, 1);
}

static void k_SuspendThread(rc_cpu *c)
{
    int n = rc_thread_suspend(RC_ARG(c, 0), 1);
    RC_RETURN(c, (uint32_t)n, 1);
}

static void k_GetLastError(rc_cpu *c) { RC_RETURN(c, rc_last_error, 0); }

static void k_SetLastError(rc_cpu *c)
{
    rc_last_error = RC_ARG(c, 0);
    RC_RETURN(c, 0, 1);
}

/* ---- thread-local storage ---- */
#define TLS_SLOTS 64u
static uint32_t tls_used[TLS_SLOTS];
#define TLS_SLOT(i) (rc_thread_tib() + 0xe10u + 4u * (i))

static void k_TlsAlloc(rc_cpu *c)
{
    for (uint32_t i = 0; i < TLS_SLOTS; i++) {
        if (!tls_used[i]) {
            tls_used[i] = 1;
            rc_st32(TLS_SLOT(i), 0);
            RC_RETURN(c, i, 0);
        }
    }
    RC_RETURN(c, 0xffffffffu, 0);
}

static void k_TlsFree(rc_cpu *c)
{
    uint32_t i = RC_ARG(c, 0);
    if (i < TLS_SLOTS) {
        tls_used[i] = 0;
    }
    RC_RETURN(c, i < TLS_SLOTS, 1);
}

static void k_TlsGetValue(rc_cpu *c)
{
    uint32_t i = RC_ARG(c, 0);
    rc_last_error = ERROR_SUCCESS;
    RC_RETURN(c, i < TLS_SLOTS ? rc_ld32(TLS_SLOT(i)) : 0, 1);
}

static void k_TlsSetValue(rc_cpu *c)
{
    uint32_t i = RC_ARG(c, 0);
    if (i < TLS_SLOTS) {
        rc_st32(TLS_SLOT(i), RC_ARG(c, 1));
    }
    RC_RETURN(c, i < TLS_SLOTS, 2);
}

/* ---- synchronisation: one thread ---- */
static void k_InitializeCriticalSection(rc_cpu *c)
{
    memset(rc_ptr(RC_ARG(c, 0)), 0, 24);
    rc_st32(RC_ARG(c, 0) + 4, 0xffffffffu); /* LockCount */
    RC_RETURN(c, 0, 1);
}

static void k_CriticalSectionNop(rc_cpu *c) { RC_RETURN(c, 0, 1); }

static void k_InterlockedIncrement(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0), v = rc_ld32(p) + 1;
    rc_st32(p, v);
    RC_RETURN(c, v, 1);
}

static void k_InterlockedDecrement(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0), v = rc_ld32(p) - 1;
    rc_st32(p, v);
    RC_RETURN(c, v, 1);
}

/* ---- time ---- */
/* Reading the clock lets other threads run: code waiting for a thread
 * typically looks at the time while it waits. */
static void k_GetTickCount(rc_cpu *c)
{
    rc_thread_yield();
    rc_thread_poll();
    RC_RETURN(c, (uint32_t)rc_ticks_ms(), 0);
}

static void k_Sleep(rc_cpu *c)
{
    rc_thread_sleep(RC_ARG(c, 0));
    RC_RETURN(c, 0, 1);
}

/* The date: the host's, or with the virtual clock a fixed one (2003-06-01,
 * midnight) plus its time, so that runs repeat. In 100 ns since 1601. */
static uint64_t now_filetime(void)
{
    const uint64_t to_1970 = 116444736000000000ull;
    if (rc_clock_virtual()) {
        return to_1970 + (1054425600ull * 1000 + rc_ticks_peek_ms()) * 10000ull;
    }
    return to_1970 + (uint64_t)time(NULL) * 10000000ull;
}

/* SYSTEMTIME from a FILETIME value. */
static void put_systemtime(uint32_t st, uint64_t ft)
{
    uint64_t ms_all = ft / 10000ull;
    uint64_t days = ms_all / 86400000ull;
    uint32_t ms_day = (uint32_t)(ms_all % 86400000ull);
    uint32_t dow = (uint32_t)((days + 1) % 7); /* 1601-01-01 was a Monday */
    /* Days since 1601-01-01 to a civil date. */
    int64_t z = (int64_t)days - 134774 + 719468; /* to days since 0000-03-01 */
    int64_t era = z / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    rc_st16(st, (uint16_t)y);
    rc_st16(st + 2, (uint16_t)m);
    rc_st16(st + 4, (uint16_t)dow);
    rc_st16(st + 6, (uint16_t)d);
    rc_st16(st + 8, (uint16_t)(ms_day / 3600000u));
    rc_st16(st + 10, (uint16_t)(ms_day / 60000u % 60u));
    rc_st16(st + 12, (uint16_t)(ms_day / 1000u % 60u));
    rc_st16(st + 14, (uint16_t)(ms_day % 1000u));
}

/* SYSTEMTIME at `st` as a FILETIME (100 ns since 1601). */
static uint64_t get_systemtime(uint32_t st)
{
    int64_t y = rc_ld16(st), m = rc_ld16(st + 2), d = rc_ld16(st + 6);
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;      /* since 1970-01-01 */
    int64_t ms = ((int64_t)rc_ld16(st + 8) * 3600 + rc_ld16(st + 10) * 60 + rc_ld16(st + 12)) * 1000 +
                 rc_ld16(st + 14);
    return (uint64_t)((days + 134774) * 86400000 + ms) * 10000u; /* 1601 to 1970: 134774 days */
}

static void k_SystemTimeToFileTime(rc_cpu *c)
{
    uint64_t ft = get_systemtime(RC_ARG(c, 0));
    rc_st32(RC_ARG(c, 1), (uint32_t)ft);
    rc_st32(RC_ARG(c, 1) + 4, (uint32_t)(ft >> 32));
    RC_RETURN(c, 1, 2);
}

static void k_CompareFileTime(rc_cpu *c)
{
    uint32_t a = RC_ARG(c, 0), b = RC_ARG(c, 1);
    uint64_t x = (uint64_t)rc_ld32(a) | (uint64_t)rc_ld32(a + 4) << 32;
    uint64_t y = (uint64_t)rc_ld32(b) | (uint64_t)rc_ld32(b + 4) << 32;
    RC_RETURN(c, x < y ? 0xffffffffu : x > y ? 1u : 0u, 2);
}

/* GetDateFormatA(locale, flags, time, format, out, size) and GetTimeFormatA: the time
 * given or now, in the US formats ("M/d/yyyy", "h:mm:ss tt"; DATE_LONGDATE as
 * "dddd, MMMM dd, yyyy"); pictures in `format` are not interpreted. */
static void date_time(rc_cpu *c, bool date)
{
    uint32_t flags = RC_ARG(c, 1), st = RC_ARG(c, 2), out = RC_ARG(c, 4), size = RC_ARG(c, 5);
    uint32_t tmp = 0;
    if (!st) {
        tmp = rc_heap_alloc(16, true);
        put_systemtime(tmp, now_filetime());
        st = tmp;
    }
    static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                       "Friday", "Saturday"};
    static const char *const months[] = {"January", "February", "March", "April", "May", "June", "July",
                                         "August", "September", "October", "November", "December"};
    unsigned y = rc_ld16(st), mo = rc_ld16(st + 2), dow = rc_ld16(st + 4), d = rc_ld16(st + 6);
    unsigned h = rc_ld16(st + 8), mi = rc_ld16(st + 10), se = rc_ld16(st + 12);
    char s[64];
    if (date && (flags & 2u)) { /* DATE_LONGDATE */
        snprintf(s, sizeof s, "%s, %s %02u, %u", days[dow % 7], months[(mo + 11) % 12], d, y);
    } else if (date) {
        snprintf(s, sizeof s, "%u/%u/%u", mo, d, y);
    } else {
        snprintf(s, sizeof s, "%u:%02u:%02u %s", h % 12 ? h % 12 : 12, mi, se, h < 12 ? "AM" : "PM");
    }
    if (tmp) {
        rc_heap_free(tmp);
    }
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (size == 0) {
        RC_RETURN(c, n, 6);
    }
    if (size < n) {
        rc_last_error = 122; /* ERROR_INSUFFICIENT_BUFFER */
        RC_RETURN(c, 0, 6);
    }
    rc_put_str(out, size, s);
    RC_RETURN(c, n, 6);
}

static void k_GetDateFormatA(rc_cpu *c) { date_time(c, true); }
static void k_GetTimeFormatA(rc_cpu *c) { date_time(c, false); }

static void k_GetComputerNameA(rc_cpu *c)
{
    const char *name = "RECOMP";
    uint32_t buf = RC_ARG(c, 0), psize = RC_ARG(c, 1);
    uint32_t n = (uint32_t)strlen(name);
    if (rc_ld32(psize) <= n) {
        rc_st32(psize, n + 1);
        rc_last_error = 111; /* ERROR_BUFFER_OVERFLOW */
        RC_RETURN(c, 0, 2);
    }
    rc_put_str(buf, n + 1, name);
    rc_st32(psize, n);
    RC_RETURN(c, 1, 2);
}

/* Guest threads take turns; their priorities change nothing. */
static void k_SetThreadPriority(rc_cpu *c) { RC_RETURN(c, 1, 2); }

static void k_lstrlenW(rc_cpu *c)
{
    uint32_t s = RC_ARG(c, 0), n = 0;
    while (s && rc_ld16(s + 2 * n)) {
        n++;
    }
    RC_RETURN(c, n, 1);
}

static void k_lstrlenA(rc_cpu *c)
{
    uint32_t s = RC_ARG(c, 0);
    RC_RETURN(c, s ? (uint32_t)strlen(rc_str(s)) : 0, 1);
}

/* Local time is UTC here (GetTimeZoneInformation says so). */
static void k_GetSystemTime(rc_cpu *c)
{
    put_systemtime(RC_ARG(c, 0), now_filetime());
    RC_RETURN(c, 0, 1);
}

/* GetSystemTimeAsFileTime(ft) */
static void k_GetSystemTimeAsFileTime(rc_cpu *c)
{
    uint64_t t = now_filetime();
    rc_st32(RC_ARG(c, 0), (uint32_t)t);
    rc_st32(RC_ARG(c, 0) + 4, (uint32_t)(t >> 32));
    RC_RETURN(c, 0, 1);
}

/* GetSystemInfo(info): one Pentium II class processor (as cpuid says), 4 KiB pages,
 * 64 KiB allocation granularity, the 2 GiB user address space. */
static void k_GetSystemInfo(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    rc_st16(p + 0, 0);              /* PROCESSOR_ARCHITECTURE_INTEL */
    rc_st16(p + 2, 0);
    rc_st32(p + 4, 0x1000);         /* page size */
    rc_st32(p + 8, 0x00010000);     /* lowest application address */
    rc_st32(p + 12, 0x7ffeffff);    /* highest */
    rc_st32(p + 16, 1);             /* active processor mask */
    rc_st32(p + 20, 1);             /* processors */
    rc_st32(p + 24, 586);           /* PROCESSOR_INTEL_PENTIUM */
    rc_st32(p + 28, 0x10000);       /* allocation granularity */
    rc_st16(p + 32, 6);             /* processor level (family) */
    rc_st16(p + 34, 0x0803);        /* model 8, stepping 3 */
    RC_RETURN(c, 0, 1);
}

/* OpenProcess(access, inherit, pid): only the program's own process (id 1) exists. */
static void k_OpenProcess(rc_cpu *c)
{
    if (RC_ARG(c, 2) == 1) {
        RC_RETURN(c, 0xffffffffu, 3);
    }
    rc_last_error = ERROR_INVALID_PARAMETER;
    RC_RETURN(c, 0, 3);
}

/* CreateProcessA(app, cmdline, …, startup, info): no other programs run here. */
static void k_CreateProcessA(rc_cpu *c)
{
    uint32_t app = RC_ARG(c, 0), cmd = RC_ARG(c, 1);
    rc_log("kernel32: CreateProcessA(%s, %s) refused", app ? rc_str(app) : "-", cmd ? rc_str(cmd) : "-");
    rc_last_error = 2; /* ERROR_FILE_NOT_FOUND */
    RC_RETURN(c, 0, 10);
}

static void k_FileTimeToSystemTime(rc_cpu *c)
{
    uint32_t ft = RC_ARG(c, 0);
    put_systemtime(RC_ARG(c, 1), (uint64_t)rc_ld32(ft) | (uint64_t)rc_ld32(ft + 4) << 32);
    RC_RETURN(c, 1, 2);
}

static void k_FileTimeToLocalFileTime(rc_cpu *c)
{
    uint32_t in = RC_ARG(c, 0), out = RC_ARG(c, 1);
    uint32_t lo = rc_ld32(in), hi = rc_ld32(in + 4);
    rc_st32(out, lo);
    rc_st32(out + 4, hi);
    RC_RETURN(c, 1, 2);
}

/* No MMX, SSE or 3DNow!: programs take their plain x86 paths, which is
 * what the lifter models. */
static void k_IsProcessorFeaturePresent(rc_cpu *c) { RC_RETURN(c, 0, 1); }

static void k_SetUnhandledExceptionFilter(rc_cpu *c) { RC_RETURN(c, 0, 1); }

/* No message texts here. */
static void k_FormatMessageA(rc_cpu *c) { RC_RETURN(c, 0, 7); }

static void k_OutputDebugStringA(rc_cpu *c)
{
    uint32_t s = RC_ARG(c, 0);
    if (s && rc_trace("debug")) {
        rc_log("[debug] %s", rc_str(s));
    }
    RC_RETURN(c, 0, 1);
}

static void k_InterlockedExchange(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0), v = RC_ARG(c, 1);
    uint32_t old = rc_ld32(p);
    rc_st32(p, v);
    RC_RETURN(c, old, 2);
}

static void k_GetTimeZoneInformation(rc_cpu *c)
{
    memset(rc_ptr(RC_ARG(c, 0)), 0, 172);
    RC_RETURN(c, 0, 1); /* TIME_ZONE_ID_UNKNOWN: no daylight saving */
}

/* ---- environment ---- */
#define ENV_MAX 64
static char env[ENV_MAX][256];
static int nenv;
static uint32_t env_block_a, env_block_w;

static void env_init(void)
{
    if (nenv) {
        return;
    }
    static const char *const init[] = {
        "ALLUSERSPROFILE=C:\\Documents and Settings\\All Users",
        "COMSPEC=C:\\WINDOWS\\system32\\cmd.exe",
        "OS=Windows_NT",
        "PATH=C:\\WINDOWS\\system32;C:\\WINDOWS",
        "SYSTEMROOT=C:\\WINDOWS",
        "TEMP=C:\\WINDOWS\\TEMP",
        "TMP=C:\\WINDOWS\\TEMP",
        "WINDIR=C:\\WINDOWS",
    };
    for (unsigned i = 0; i < sizeof init / sizeof init[0]; i++) {
        snprintf(env[nenv++], sizeof env[0], "%s", init[i]);
    }
}

static void k_GetEnvironmentStrings(rc_cpu *c)
{
    env_init();
    if (!env_block_a) {
        uint32_t n = 1;
        for (int i = 0; i < nenv; i++) {
            n += (uint32_t)strlen(env[i]) + 1;
        }
        env_block_a = rc_proc_alloc(n);
        uint32_t p = env_block_a;
        for (int i = 0; i < nenv; i++) {
            p += rc_put_str(p, 256, env[i]) + 1;
        }
        rc_st8(p, 0);
    }
    RC_RETURN(c, env_block_a, 0);
}

static void k_GetEnvironmentStringsW(rc_cpu *c)
{
    env_init();
    if (!env_block_w) {
        uint32_t n = 1;
        for (int i = 0; i < nenv; i++) {
            n += (uint32_t)strlen(env[i]) + 1;
        }
        env_block_w = rc_proc_alloc(2 * n);
        uint32_t p = env_block_w;
        for (int i = 0; i < nenv; i++) {
            for (const char *s = env[i]; *s; s++) {
                rc_st16(p, (uint8_t)*s);
                p += 2;
            }
            rc_st16(p, 0);
            p += 2;
        }
        rc_st16(p, 0);
    }
    RC_RETURN(c, env_block_w, 0);
}

static void k_FreeEnvironmentStrings(rc_cpu *c) { RC_RETURN(c, 1, 1); }

static int env_find(const char *name)
{
    size_t n = strlen(name);
    for (int i = 0; i < nenv; i++) {
        if (strlen(env[i]) > n && env[i][n] == '=') {
            int same = 1;
            for (size_t k = 0; k < n; k++) {
                char a = env[i][k], b = name[k];
                if (a >= 'a' && a <= 'z') {
                    a = (char)(a - 32);
                }
                if (b >= 'a' && b <= 'z') {
                    b = (char)(b - 32);
                }
                if (a != b) {
                    same = 0;
                    break;
                }
            }
            if (same) {
                return i;
            }
        }
    }
    return -1;
}

static void k_GetEnvironmentVariableA(rc_cpu *c)
{
    env_init();
    int i = env_find(rc_str(RC_ARG(c, 0)));
    if (i < 0) {
        rc_last_error = ERROR_ENVVAR_NOT_FOUND;
        RC_RETURN(c, 0, 3);
    }
    const char *v = strchr(env[i], '=') + 1;
    uint32_t len = (uint32_t)strlen(v);
    if (len + 1 > RC_ARG(c, 2)) {
        RC_RETURN(c, len + 1, 3);
    }
    RC_RETURN(c, rc_put_str(RC_ARG(c, 1), RC_ARG(c, 2), v), 3);
}

static void k_SetEnvironmentVariableA(rc_cpu *c)
{
    env_init();
    const char *name = rc_str(RC_ARG(c, 0)), *value = rc_str(RC_ARG(c, 1));
    int i = env_find(name);
    if (value == NULL) {
        if (i >= 0) {
            memmove(env[i], env[i + 1], sizeof env[0] * (size_t)(nenv - i - 1));
            nenv--;
        }
    } else {
        if (i < 0) {
            if (nenv == ENV_MAX) {
                RC_RETURN(c, 0, 2);
            }
            i = nenv++;
        }
        snprintf(env[i], sizeof env[0], "%s=%s", name, value);
    }
    RC_RETURN(c, 1, 2);
}

static void k_SetConsoleCtrlHandler(rc_cpu *c) { RC_RETURN(c, 1, 2); }

static void k_UnhandledExceptionFilter(rc_cpu *c)
{
    rc_log("UnhandledExceptionFilter: the program crashed");
    RC_RETURN(c, 1, 1); /* EXCEPTION_EXECUTE_HANDLER: the process ends */
}

/* ---- structured exceptions ----
 *
 * Win32 SEH on the guest's own registration records (fs:[0], a chain on the stack): the
 * handlers are the program's own lifted code (MSVC's __CxxFrameHandler for C++ exceptions,
 * __except_handler3 for __try). When a handler continues in an outer frame (it jumps there with
 * the stack cut back), the runtime unwinds the lifted functions to that frame (rc_dispatch_jump,
 * rc_unwind_claim in rc/lifted.h). */

#define EXCEPTION_NONCONTINUABLE 0x1u
#define EXCEPTION_UNWINDING 0x2u
#define EXCEPTION_EXIT_UNWIND 0x4u
#define CHAIN_END 0xffffffffu
#define EXCEPTION_RECORD_SIZE 0x50u
#define CONTEXT_SIZE 0x2ccu

/* EXCEPTION_RECORD and CONTEXT at `rec` and `ctx` for an exception raised by the caller of the
 * current import (registers as they will be after it returns, `args` bytes of arguments). */
static void fill_exception(rc_cpu *c, uint32_t rec, uint32_t ctx, uint32_t code, uint32_t flags, uint32_t n,
                           uint32_t info, uint32_t args)
{
    uint32_t ret = rc_ld32(c->r[RC_ESP]);
    for (uint32_t i = 0; i < EXCEPTION_RECORD_SIZE; i += 4) {
        rc_st32(rec + i, 0);
    }
    rc_st32(rec + 0, code);
    rc_st32(rec + 4, flags);
    rc_st32(rec + 12, ret);
    n = n > 15 ? 15 : n;
    rc_st32(rec + 16, n);
    for (uint32_t i = 0; i < n; i++) {
        rc_st32(rec + 20 + 4 * i, info ? rc_ld32(info + 4 * i) : 0);
    }
    for (uint32_t i = 0; i < CONTEXT_SIZE; i += 4) {
        rc_st32(ctx + i, 0);
    }
    rc_st32(ctx + 0x00, 0x1003fu); /* CONTEXT_FULL and the debug, float and extended registers */
    rc_st32(ctx + 0x9c, c->r[RC_EDI]);
    rc_st32(ctx + 0xa0, c->r[RC_ESI]);
    rc_st32(ctx + 0xa4, c->r[RC_EBX]);
    rc_st32(ctx + 0xa8, c->r[RC_EDX]);
    rc_st32(ctx + 0xac, c->r[RC_ECX]);
    rc_st32(ctx + 0xb0, c->r[RC_EAX]);
    rc_st32(ctx + 0xb4, c->r[RC_EBP]);
    rc_st32(ctx + 0xb8, ret);
    rc_st32(ctx + 0xbc, 0x1b);
    rc_st32(ctx + 0xc0, rc_eflags(c));
    rc_st32(ctx + 0xc4, c->r[RC_ESP] + 4 + args);
    rc_st32(ctx + 0xc8, 0x23);
}

#define EXCEPTION_NESTED_CALL 0x10u
#define DISPOSITION_NESTED 2

/* The handler of the dispatcher's own registration record (below): an exception raised while a
 * handler runs (in a catch block) reaches it as nested; it names the frame whose handler was
 * running. Unwinding passes it by. */
static void k_rc_NestedExceptionHandler(rc_cpu *c)
{
    uint32_t rec = RC_ARG(c, 0), frame = RC_ARG(c, 1), dc = RC_ARG(c, 3);
    if (rc_ld32(rec + 4) & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        RC_RETURN(c, 1, 0); /* ExceptionContinueSearch (cdecl) */
    }
    rc_st32(dc, rc_ld32(frame + 8));
    RC_RETURN(c, DISPOSITION_NESTED, 0);
}

/* Call an exception handler (cdecl: handler(record, frame, context, dispatcher context)) with
 * the stack below `low`; its result, or -1 when the exception continues elsewhere (unwinding).
 * As Windows does, a registration record of the dispatcher's own is on top of the chain while
 * the handler runs (next, handler, the frame being handled): MSVC's __CxxFrameHandler relinks
 * it above the catching frame after RtlUnwind, and nested exceptions find it. */
static int call_handler(rc_cpu *c, uint32_t handler, uint32_t low, uint32_t rec, uint32_t frame, uint32_t ctx,
                        uint32_t dc)
{
    uint32_t mine = low;
    rc_st32(mine, rc_ld32(c->fs_base));
    rc_st32(mine + 4, rc_import_address(K32, "rc_NestedExceptionHandler"));
    rc_st32(mine + 8, frame);
    rc_st32(c->fs_base, mine);
    c->r[RC_ESP] = mine - 16;
    rc_st32(dc, 0);
    uint32_t a[4] = {rec, frame, ctx, dc};
    uint32_t r = rc_call_guest(c, handler, 4, a);
    if (c->unwind) {
        return -1;
    }
    rc_st32(c->fs_base, rc_ld32(mine)); /* what the record links to now */
    return (int)r;
}

/* The C++ type of a thrown MSVC exception (0xe06d7363), for the message when nothing catches it. */
static const char *thrown_type(uint32_t rec)
{
    if (rc_ld32(rec) != 0xe06d7363u || rc_ld32(rec + 16) < 3) {
        return "";
    }
    uint32_t info = rc_ld32(rec + 28); /* ThrowInfo */
    uint32_t types = info ? rc_ld32(info + 12) : 0;
    uint32_t first = types && rc_ld32(types) ? rc_ld32(types + 4) : 0; /* CatchableType */
    uint32_t td = first ? rc_ld32(first + 4) : 0;
    return td ? rc_str(td + 8) : "";
}

/* RaiseException(code, flags, count, arguments): the handlers of the chain from fs:[0] are asked
 * in turn; one may continue in its own frame further up (the usual end of a C++ throw), or
 * resume after the call (ExceptionContinueExecution). */
static void k_RaiseException(rc_cpu *c)
{
    uint32_t code = RC_ARG(c, 0), flags = RC_ARG(c, 1) & EXCEPTION_NONCONTINUABLE;
    uint32_t n = RC_ARG(c, 2), info = RC_ARG(c, 3);
    uint32_t entry = c->r[RC_ESP];
    uint32_t ctx = (entry - 0x40 - CONTEXT_SIZE) & ~0xfu;
    uint32_t rec = ctx - EXCEPTION_RECORD_SIZE;
    uint32_t dc = rec - 16;
    uint32_t low = dc - 64;
    fill_exception(c, rec, ctx, code, flags, n, info, 16);
    uint32_t saved_eh_esp = c->eh_esp;
    uint32_t nested = 0; /* a frame whose handler raised this exception, when nested */
    c->eh_depth++;
    c->eh_esp = entry;
    if (getenv("RC_TRACE_SEH")) {
        rc_log("seh: raise %#x %s at %#010x esp %#x depth %u", code, thrown_type(rec), rc_ld32(entry), entry, c->eh_depth);
        for (uint32_t f = rc_ld32(c->fs_base), i = 0; f != CHAIN_END && f && i < 20; f = rc_ld32(f), i++) {
            rc_log("seh:   frame %#x handler %#010x [+8] %#x [+c] %#x", f, rc_ld32(f + 4), rc_ld32(f + 8), rc_ld32(f + 12));
        }
    }
    for (uint32_t frame = rc_ld32(c->fs_base); frame != CHAIN_END && frame;) {
        int r = call_handler(c, rc_ld32(frame + 4), low, rec, frame, ctx, dc);
        if (getenv("RC_TRACE_SEH")) {
            rc_log("seh: frame %#x -> %d (unwind %u to %#010x ebp %#x)", frame, r, c->unwind, c->unwind_eip, c->unwind_ebp);
        }
        if (r < 0) {
            c->eh_depth--;
            c->eh_esp = saved_eh_esp;
            return; /* continues in an outer frame */
        }
        if (r == 0) { /* ExceptionContinueExecution */
            c->eh_depth--;
            c->eh_esp = saved_eh_esp;
            if (flags & EXCEPTION_NONCONTINUABLE) {
                rc_fatal("RaiseException(%#x): a handler continued a noncontinuable exception", code);
            }
            c->r[RC_ESP] = entry;
            RC_RETURN(c, 0, 4);
        }
        if (frame == nested) {
            rc_st32(rec + 4, rc_ld32(rec + 4) & ~EXCEPTION_NESTED_CALL);
            nested = 0;
        }
        if (r == DISPOSITION_NESTED) {
            rc_st32(rec + 4, rc_ld32(rec + 4) | EXCEPTION_NESTED_CALL);
            if (rc_ld32(dc) > nested) {
                nested = rc_ld32(dc);
            }
        }
        frame = rc_ld32(frame); /* ExceptionContinueSearch, or nested: the next record */
    }
    rc_fatal("unhandled exception %#x %s raised from %#010x", code, thrown_type(rec), rc_ld32(entry));
}

/* RtlUnwind(target frame, target ip, record, return value): every handler of the chain above
 * the target frame is called once more to unwind (destructors, __finally), and its record taken
 * off the chain; then the call returns (to the target ip, which is the return address). */
static void k_RtlUnwind(rc_cpu *c)
{
    uint32_t target = RC_ARG(c, 0), given = RC_ARG(c, 2), value = RC_ARG(c, 3);
    uint32_t entry = c->r[RC_ESP];
    uint32_t ctx = (entry - 0x40 - CONTEXT_SIZE) & ~0xfu;
    uint32_t rec = ctx - EXCEPTION_RECORD_SIZE;
    uint32_t dc = rec - 16;
    uint32_t low = dc - 64;
    fill_exception(c, rec, ctx, 0xc0000027u /* STATUS_UNWIND */, 0, 0, 0, 16);
    if (given) {
        for (uint32_t i = 0; i < EXCEPTION_RECORD_SIZE; i += 4) {
            rc_st32(rec + i, rc_ld32(given + i));
        }
    }
    rc_st32(rec + 4, rc_ld32(rec + 4) | EXCEPTION_UNWINDING | (target ? 0 : EXCEPTION_EXIT_UNWIND));
    if (getenv("RC_TRACE_SEH")) {
        rc_log("seh: unwind to frame %#x", target);
    }
    for (uint32_t frame = rc_ld32(c->fs_base); frame != CHAIN_END && frame && frame != target;) {
        int r = call_handler(c, rc_ld32(frame + 4), low, rec, frame, ctx, dc);
        if (r < 0) {
            return;
        }
        frame = rc_ld32(frame);
        rc_st32(c->fs_base, frame);
    }
    if (given) {
        rc_st32(given + 4, rc_ld32(rec + 4));
    }
    c->r[RC_ESP] = entry;
    RC_RETURN(c, value, 4);
}

const rc_import_def rc_k32_process[] = {
    {K32, "GetVersion", k_GetVersion},
    {K32, "GetSystemTime", k_GetSystemTime},
    {K32, "GetSystemTimeAsFileTime", k_GetSystemTimeAsFileTime},
    {K32, "GetSystemInfo", k_GetSystemInfo},
    {K32, "OpenProcess", k_OpenProcess},
    {K32, "CreateProcessA", k_CreateProcessA},
    {K32, "SystemTimeToFileTime", k_SystemTimeToFileTime},
    {K32, "CompareFileTime", k_CompareFileTime},
    {K32, "GetDateFormatA", k_GetDateFormatA},
    {K32, "GetTimeFormatA", k_GetTimeFormatA},
    {K32, "GetComputerNameA", k_GetComputerNameA},
    {K32, "lstrlenA", k_lstrlenA},
    {K32, "lstrlenW", k_lstrlenW},
    {K32, "SetThreadPriority", k_SetThreadPriority},
    {K32, "GetLocalTime", k_GetSystemTime},
    {K32, "FileTimeToSystemTime", k_FileTimeToSystemTime},
    {K32, "FileTimeToLocalFileTime", k_FileTimeToLocalFileTime},
    {K32, "SetUnhandledExceptionFilter", k_SetUnhandledExceptionFilter},
    {K32, "IsProcessorFeaturePresent", k_IsProcessorFeaturePresent},
    {K32, "FormatMessageA", k_FormatMessageA},
    {K32, "OutputDebugStringA", k_OutputDebugStringA},
    {K32, "InterlockedExchange", k_InterlockedExchange},
    {K32, "GetVersionExA", k_GetVersionExA},
    {K32, "GetCommandLineA", k_GetCommandLineA},
    {K32, "GetStartupInfoA", k_GetStartupInfoA},
    {K32, "GetModuleHandleA", k_GetModuleHandleA},
    {K32, "LoadLibraryA", k_LoadLibraryA},
    {K32, "FreeLibrary", k_FreeLibrary},
    {K32, "GetProcAddress", k_GetProcAddress},
    {K32, "GetModuleFileNameA", k_GetModuleFileNameA},
    {K32, "GetWindowsDirectoryA", k_GetWindowsDirectoryA},
    {K32, "GetSystemDirectoryA", k_GetSystemDirectoryA},
    {K32, "SetErrorMode", k_SetErrorMode},
    {K32, "GetCurrentProcessId", k_GetCurrentProcessId},
    {K32, "QueryPerformanceFrequency", k_QueryPerformanceFrequency},
    {K32, "QueryPerformanceCounter", k_QueryPerformanceCounter},
    {K32, "ExitProcess", k_ExitProcess},
    {K32, "TerminateProcess", k_TerminateProcess},
    {K32, "FatalAppExitA", k_FatalAppExitA},
    {K32, "GetCurrentProcess", k_GetCurrentProcess},
    {K32, "GetCurrentThread", k_GetCurrentThread},
    {K32, "GetCurrentThreadId", k_GetCurrentThreadId},
    {K32, "DisableThreadLibraryCalls", k_DisableThreadLibraryCalls},
    {K32, "CreateThread", k_CreateThread},
    {K32, "ResumeThread", k_ResumeThread},
    {K32, "SuspendThread", k_SuspendThread},
    {K32, "GetLastError", k_GetLastError},
    {K32, "SetLastError", k_SetLastError},
    {K32, "TlsAlloc", k_TlsAlloc},
    {K32, "TlsFree", k_TlsFree},
    {K32, "TlsGetValue", k_TlsGetValue},
    {K32, "TlsSetValue", k_TlsSetValue},
    {K32, "InitializeCriticalSection", k_InitializeCriticalSection},
    {K32, "DeleteCriticalSection", k_CriticalSectionNop},
    {K32, "EnterCriticalSection", k_CriticalSectionNop},
    {K32, "LeaveCriticalSection", k_CriticalSectionNop},
    {K32, "InterlockedIncrement", k_InterlockedIncrement},
    {K32, "InterlockedDecrement", k_InterlockedDecrement},
    {K32, "GetTickCount", k_GetTickCount},
    {K32, "Sleep", k_Sleep},
    {K32, "GetTimeZoneInformation", k_GetTimeZoneInformation},
    {K32, "GetEnvironmentStrings", k_GetEnvironmentStrings},
    {K32, "GetEnvironmentStringsW", k_GetEnvironmentStringsW},
    {K32, "FreeEnvironmentStringsA", k_FreeEnvironmentStrings},
    {K32, "FreeEnvironmentStringsW", k_FreeEnvironmentStrings},
    {K32, "GetEnvironmentVariableA", k_GetEnvironmentVariableA},
    {K32, "SetEnvironmentVariableA", k_SetEnvironmentVariableA},
    {K32, "SetConsoleCtrlHandler", k_SetConsoleCtrlHandler},
    {K32, "UnhandledExceptionFilter", k_UnhandledExceptionFilter},
    {K32, "RaiseException", k_RaiseException},
    {K32, "RtlUnwind", k_RtlUnwind},
    {K32, "rc_NestedExceptionHandler", k_rc_NestedExceptionHandler},
};
const uint32_t rc_k32_process_count = sizeof rc_k32_process / sizeof rc_k32_process[0];
