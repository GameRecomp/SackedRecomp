/* SPDX-License-Identifier: MIT */
/*
 * MSVCRT: stdio. A FILE is a guest structure in MSVC 6's layout (_iob for the standard streams,
 * a heap block for fopen's), so code that reads its fields finds them; what it does is kept
 * here. The bytes come and go through the guest's kernel32 (CreateFileA, ReadFile, WriteFile,
 * SetFilePointer), which knows where the game's and the user's files are. Reading is buffered on
 * the host; writing is not. Text mode turns CR LF into LF when reading and LF into CR LF when
 * writing; ftell gives the file position, as MSVC does.
 *
 * __pioinfo (the low-level I/O table) is there for STLport, which reads the flags of the
 * standard descriptors from it.
 */
#include "msvcrt.h"

#include <stdlib.h>
#include <string.h>

#include "rc/log.h"
#include "rc/mem.h"
#include "rc/vm.h"

#define K32 "KERNEL32.dll"

#define IOB_ENTRIES 20u
#define FILE_SIZE 32u
#define IOINFO_SIZE 36u /* osfhnd, osfile, pipech, lock (the multithreaded runtime's) */
#define IOINFO_ELTS 32u
#define IOINFO_ARRAYS 64u

#define F_IOREAD 0x01u
#define F_IOWRT 0x02u
#define F_IOEOF 0x10u
#define F_IOERR 0x20u
#define F_IORW 0x80u

#define FOPEN 0x01
#define FDEV 0x40
#define FTEXT 0x80

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define INVALID_HANDLE 0xffffffffu

#define BUF 65536u
#define FILES 64

typedef struct file {
    uint32_t guest; /* the FILE */
    uint32_t handle;
    int fd;
    bool text, readable, writable, append, eof, err;
    uint8_t *buf;
    uint32_t buf_len, buf_pos;
    uint32_t buf_at; /* file position of buf[0] */
    int unget;       /* a pushed-back character, or -1 */
} file;

static file files[FILES];
static uint32_t iob, pioinfo, ioinfo0, scratch, scratch_count;

static void setup(void)
{
    if (iob) {
        return;
    }
    iob = rc_heap_alloc(IOB_ENTRIES * FILE_SIZE, true);
    ioinfo0 = rc_heap_alloc(IOINFO_ELTS * IOINFO_SIZE, true);
    pioinfo = rc_heap_alloc(IOINFO_ARRAYS * 4, true);
    rc_st32(pioinfo, ioinfo0);
    scratch = rc_heap_alloc(BUF, false);
    scratch_count = rc_heap_alloc(8, true);
    for (int i = 0; i < 3; i++) {
        uint32_t f = iob + (uint32_t)i * FILE_SIZE;
        rc_st32(f + 12, i == 0 ? F_IOREAD : F_IOWRT);
        rc_st32(f + 16, (uint32_t)i);
        rc_st32(ioinfo0 + (uint32_t)i * IOINFO_SIZE, INVALID_HANDLE);
        rc_st8(ioinfo0 + (uint32_t)i * IOINFO_SIZE + 4, FOPEN | FDEV | FTEXT);
        file *h = &files[i];
        memset(h, 0, sizeof *h);
        h->guest = f;
        h->handle = INVALID_HANDLE;
        h->fd = i;
        h->text = true;
        h->readable = i == 0;
        h->writable = i != 0;
        h->unget = -1;
    }
}

static uint32_t var_iob(void)
{
    setup();
    return iob;
}

static uint32_t var_pioinfo(void)
{
    setup();
    return pioinfo;
}

static file *find(uint32_t guest)
{
    setup();
    for (int i = 0; i < FILES; i++) {
        if (files[i].guest == guest && guest) {
            return &files[i];
        }
    }
    return NULL;
}

static uint32_t k32(rc_cpu *c, const char *name, uint32_t n, const uint32_t *a)
{
    return rc_crt_call(c, rc_import_address(K32, name), n, a);
}

/* The file position (32 bits: the games' files are small). */
static uint32_t os_seek(rc_cpu *c, file *f, int32_t off, uint32_t method)
{
    uint32_t a[4] = {f->handle, (uint32_t)off, 0, method};
    return k32(c, "SetFilePointer", 4, a);
}

/* Drop what is buffered for reading: the OS position goes back to the logical one. */
static void drop_buffer(rc_cpu *c, file *f)
{
    if (f->buf_len && f->buf_pos < f->buf_len) {
        os_seek(c, f, (int32_t)(f->buf_at + f->buf_pos), 0);
    }
    f->buf_at += f->buf_pos;
    f->buf_len = f->buf_pos = 0;
    f->unget = -1;
}

static bool fill(rc_cpu *c, file *f)
{
    if (!f->readable || f->handle == INVALID_HANDLE) {
        return false;
    }
    if (!f->buf) {
        f->buf = malloc(BUF);
    }
    f->buf_at += f->buf_len;
    f->buf_len = f->buf_pos = 0;
    uint32_t a[5] = {f->handle, scratch, BUF, scratch_count, 0};
    if (!k32(c, "ReadFile", 5, a)) {
        f->err = true;
        return false;
    }
    uint32_t got = rc_ld32(scratch_count);
    memcpy(f->buf, rc_ptr(scratch), got);
    f->buf_len = got;
    if (!got) {
        f->eof = true;
    }
    return got > 0;
}

/* The next raw byte, or -1 at the end. */
static int raw_byte(rc_cpu *c, file *f)
{
    if (f->buf_pos >= f->buf_len && !fill(c, f)) {
        return -1;
    }
    return f->buf[f->buf_pos++];
}

/* The next character (text mode: CR LF is LF), or -1. */
static int get_char(rc_cpu *c, file *f)
{
    if (f->unget >= 0) {
        int ch = f->unget;
        f->unget = -1;
        return ch;
    }
    int ch = raw_byte(c, f);
    if (f->text && ch == '\r') {
        int next = raw_byte(c, f);
        if (next == '\n') {
            return '\n';
        }
        if (next >= 0) {
            f->buf_pos--;
        }
    }
    if (f->text && ch == 0x1a) {
        f->eof = true;
        return -1;
    }
    return ch;
}

static uint32_t logical_pos(rc_cpu *c, file *f)
{
    if (f->buf_len) {
        return f->buf_at + f->buf_pos - (f->unget >= 0 ? 1u : 0u);
    }
    return os_seek(c, f, 0, 1) - (f->unget >= 0 ? 1u : 0u);
}

/* Write guest bytes (text mode: LF becomes CR LF). */
static uint32_t write_bytes(rc_cpu *c, file *f, uint32_t src, uint32_t n)
{
    if (!f->writable || f->handle == INVALID_HANDLE) {
        if (f->fd == 1 || f->fd == 2) { /* stdout, stderr: to the log */
            char *tmp = malloc(n + 1);
            memcpy(tmp, rc_ptr(src), n);
            tmp[n] = 0;
            rc_log("%s", tmp);
            free(tmp);
            return n;
        }
        f->err = true;
        return 0;
    }
    drop_buffer(c, f);
    if (f->append) {
        os_seek(c, f, 0, 2);
    }
    if (!f->text) {
        uint32_t a[5] = {f->handle, src, n, scratch_count, 0};
        k32(c, "WriteFile", 5, a);
        uint32_t done = rc_ld32(scratch_count);
        f->buf_at = os_seek(c, f, 0, 1);
        return done;
    }
    uint32_t done = 0;
    while (done < n) {
        uint32_t k = 0;
        while (done < n && k + 2 <= BUF) {
            uint8_t b = rc_ld8(src + done++);
            if (b == '\n') {
                rc_st8(scratch + k++, '\r');
            }
            rc_st8(scratch + k++, b);
        }
        uint32_t a[5] = {f->handle, scratch, k, scratch_count, 0};
        k32(c, "WriteFile", 5, a);
    }
    f->buf_at = os_seek(c, f, 0, 1);
    return n;
}

/* ---- the imports ---- */

/* fopen(name, mode) */
static void k_fopen(rc_cpu *c)
{
    setup();
    const char *mode = rc_str(RC_ARG(c, 1));
    uint32_t access, disposition;
    bool append = false, plus = strchr(mode, '+') != NULL;
    switch (mode[0]) {
    case 'r':
        access = GENERIC_READ | (plus ? GENERIC_WRITE : 0);
        disposition = OPEN_EXISTING;
        break;
    case 'w':
        access = GENERIC_WRITE | (plus ? GENERIC_READ : 0);
        disposition = CREATE_ALWAYS;
        break;
    case 'a':
        access = GENERIC_WRITE | (plus ? GENERIC_READ : 0);
        disposition = OPEN_ALWAYS;
        append = true;
        break;
    default:
        RC_RETURN(c, 0, 0);
    }
    int slot = -1;
    for (int i = 3; i < FILES && slot < 0; i++) {
        if (!files[i].guest) {
            slot = i;
        }
    }
    if (slot < 0) {
        RC_RETURN(c, 0, 0);
    }
    uint32_t a[7] = {RC_ARG(c, 0), access, 1 /* FILE_SHARE_READ */, 0, disposition, 0x80, 0};
    uint32_t h = k32(c, "CreateFileA", 7, a);
    if (h == INVALID_HANDLE) {
        RC_RETURN(c, 0, 0);
    }
    file *f = &files[slot];
    memset(f, 0, sizeof *f);
    f->guest = rc_heap_alloc(FILE_SIZE, true);
    f->handle = h;
    f->fd = slot;
    f->text = strchr(mode, 'b') == NULL;
    f->readable = (access & GENERIC_READ) != 0;
    f->writable = (access & GENERIC_WRITE) != 0;
    f->append = append;
    f->unget = -1;
    rc_st32(f->guest + 12, (f->readable ? F_IOREAD : 0) | (f->writable ? F_IOWRT : 0) | (plus ? F_IORW : 0));
    rc_st32(f->guest + 16, (uint32_t)slot);
    if ((uint32_t)slot < IOINFO_ELTS) {
        rc_st32(ioinfo0 + (uint32_t)slot * IOINFO_SIZE, h);
        rc_st8(ioinfo0 + (uint32_t)slot * IOINFO_SIZE + 4, (uint8_t)(FOPEN | (f->text ? FTEXT : 0)));
    }
    RC_RETURN(c, f->guest, 0);
}

static void k_fclose(rc_cpu *c)
{
    file *f = find(RC_ARG(c, 0));
    if (!f || f->fd < 3) {
        RC_RETURN(c, 0xffffffffu, 0);
    }
    uint32_t a[1] = {f->handle};
    k32(c, "CloseHandle", 1, a);
    if ((uint32_t)f->fd < IOINFO_ELTS) {
        rc_st32(ioinfo0 + (uint32_t)f->fd * IOINFO_SIZE, INVALID_HANDLE);
        rc_st8(ioinfo0 + (uint32_t)f->fd * IOINFO_SIZE + 4, 0);
    }
    rc_heap_free(f->guest);
    free(f->buf);
    memset(f, 0, sizeof *f);
    RC_RETURN(c, 0, 0);
}

/* fread(buffer, size, count, file): whole items read. */
static void k_fread(rc_cpu *c)
{
    uint32_t dst = RC_ARG(c, 0), size = RC_ARG(c, 1), count = RC_ARG(c, 2);
    file *f = find(RC_ARG(c, 3));
    uint64_t want64 = (uint64_t)size * count;
    if (!f || !size || !count || want64 > 0x7fffffffu) {
        RC_RETURN(c, 0, 0);
    }
    uint32_t want = (uint32_t)want64, got = 0;
    if (f->text) {
        while (got < want) {
            int ch = get_char(c, f);
            if (ch < 0) {
                break;
            }
            rc_st8(dst + got++, (uint8_t)ch);
        }
    } else {
        if (f->unget >= 0 && got < want) {
            rc_st8(dst + got++, (uint8_t)f->unget);
            f->unget = -1;
        }
        uint32_t have = f->buf_len - f->buf_pos;
        uint32_t k = have < want - got ? have : want - got;
        if (k) {
            memcpy(rc_ptr(dst + got), f->buf + f->buf_pos, k);
            f->buf_pos += k;
            got += k;
        }
        if (got < want && f->readable && f->handle != INVALID_HANDLE) {
            /* The rest straight into the program's buffer. */
            f->buf_at += f->buf_len;
            f->buf_len = f->buf_pos = 0;
            uint32_t a[5] = {f->handle, dst + got, want - got, scratch_count, 0};
            k32(c, "ReadFile", 5, a);
            uint32_t n = rc_ld32(scratch_count);
            got += n;
            f->buf_at += n;
            if (got < want) {
                f->eof = true;
            }
        }
    }
    RC_RETURN(c, got / size, 0);
}

static void k_fwrite(rc_cpu *c)
{
    uint32_t src = RC_ARG(c, 0), size = RC_ARG(c, 1), count = RC_ARG(c, 2);
    file *f = find(RC_ARG(c, 3));
    if (!f || !size || !count) {
        RC_RETURN(c, 0, 0);
    }
    uint32_t n = write_bytes(c, f, src, size * count);
    RC_RETURN(c, n / size, 0);
}

/* fseek(file, offset, origin) */
static void k_fseek(rc_cpu *c)
{
    file *f = find(RC_ARG(c, 0));
    int32_t off = (int32_t)RC_ARG(c, 1);
    uint32_t origin = RC_ARG(c, 2);
    if (!f || f->handle == INVALID_HANDLE || origin > 2) {
        RC_RETURN(c, 0xffffffffu, 0);
    }
    if (origin == 1) {
        off += (int32_t)logical_pos(c, f);
        origin = 0;
    }
    f->buf_len = f->buf_pos = 0;
    f->unget = -1;
    uint32_t r = os_seek(c, f, off, origin);
    f->buf_at = r;
    f->eof = false;
    RC_RETURN(c, r == INVALID_HANDLE ? 0xffffffffu : 0, 0);
}

static void k_ftell(rc_cpu *c)
{
    file *f = find(RC_ARG(c, 0));
    if (!f || f->handle == INVALID_HANDLE) {
        RC_RETURN(c, 0xffffffffu, 0);
    }
    RC_RETURN(c, logical_pos(c, f), 0);
}

static void k_fgetpos(rc_cpu *c)
{
    file *f = find(RC_ARG(c, 0));
    if (!f || f->handle == INVALID_HANDLE) {
        RC_RETURN(c, 0xffffffffu, 0);
    }
    rc_st32(RC_ARG(c, 1), logical_pos(c, f));
    rc_st32(RC_ARG(c, 1) + 4, 0);
    RC_RETURN(c, 0, 0);
}

static void k_fsetpos(rc_cpu *c)
{
    file *f = find(RC_ARG(c, 0));
    if (!f || f->handle == INVALID_HANDLE) {
        RC_RETURN(c, 0xffffffffu, 0);
    }
    f->buf_len = f->buf_pos = 0;
    f->unget = -1;
    f->buf_at = os_seek(c, f, (int32_t)rc_ld32(RC_ARG(c, 1)), 0);
    f->eof = false;
    RC_RETURN(c, 0, 0);
}

static void k_fflush(rc_cpu *c) { RC_RETURN(c, 0, 0); }
static void k_setvbuf(rc_cpu *c) { RC_RETURN(c, 0, 0); }

static void k_feof(rc_cpu *c)
{
    file *f = find(RC_ARG(c, 0));
    RC_RETURN(c, f && f->eof && f->unget < 0 ? F_IOEOF : 0, 0);
}

static void k_fgetc(rc_cpu *c)
{
    file *f = find(RC_ARG(c, 0));
    int ch = f ? get_char(c, f) : -1;
    RC_RETURN(c, (uint32_t)ch, 0);
}

static void k_ungetc(rc_cpu *c)
{
    int ch = (int32_t)RC_ARG(c, 0);
    file *f = find(RC_ARG(c, 1));
    if (!f || ch < 0) {
        RC_RETURN(c, 0xffffffffu, 0);
    }
    f->unget = ch & 0xff;
    f->eof = false;
    RC_RETURN(c, (uint32_t)(ch & 0xff), 0);
}

static void k_fputc(rc_cpu *c)
{
    uint32_t ch = RC_ARG(c, 0) & 0xff;
    file *f = find(RC_ARG(c, 1));
    if (!f) {
        RC_RETURN(c, 0xffffffffu, 0);
    }
    uint32_t tmp = rc_heap_alloc(4, false);
    rc_st8(tmp, (uint8_t)ch);
    uint32_t n = write_bytes(c, f, tmp, 1);
    rc_heap_free(tmp);
    RC_RETURN(c, n ? ch : 0xffffffffu, 0);
}

/* fgets(buffer, n, file) */
static void k_fgets(rc_cpu *c)
{
    uint32_t dst = RC_ARG(c, 0);
    int32_t n = (int32_t)RC_ARG(c, 1);
    file *f = find(RC_ARG(c, 2));
    if (!f || n <= 0) {
        RC_RETURN(c, 0, 0);
    }
    int32_t k = 0;
    while (k < n - 1) {
        int ch = get_char(c, f);
        if (ch < 0) {
            break;
        }
        rc_st8(dst + (uint32_t)k++, (uint8_t)ch);
        if (ch == '\n') {
            break;
        }
    }
    rc_st8(dst + (uint32_t)k, 0);
    RC_RETURN(c, k ? dst : 0, 0);
}

static void k_fputs(rc_cpu *c)
{
    uint32_t s = RC_ARG(c, 0);
    file *f = find(RC_ARG(c, 1));
    if (!f) {
        RC_RETURN(c, 0xffffffffu, 0);
    }
    write_bytes(c, f, s, (uint32_t)strlen(rc_str(s)));
    RC_RETURN(c, 0, 0);
}

/* _fstat(fd, struct _stat*) */
static void k__fstat(rc_cpu *c)
{
    setup();
    int32_t fd = (int32_t)RC_ARG(c, 0);
    uint32_t st = RC_ARG(c, 1);
    if (fd < 0 || fd >= FILES || (!files[fd].guest)) {
        RC_RETURN(c, 0xffffffffu, 0);
    }
    memset(rc_ptr(st), 0, 36);
    file *f = &files[fd];
    if (fd < 3) {
        rc_st16(st + 6, 0x2000 | 0x0180); /* a character device, read and write */
    } else {
        uint32_t a[2] = {f->handle, 0};
        uint32_t size = k32(c, "GetFileSize", 2, a);
        rc_st16(st + 6, 0x8000 | 0x0180);
        rc_st16(st + 8, 1);
        rc_st32(st + 20, size);
    }
    RC_RETURN(c, 0, 0);
}

static void k__get_osfhandle(rc_cpu *c)
{
    setup();
    int32_t fd = (int32_t)RC_ARG(c, 0);
    if (fd < 0 || fd >= FILES || !files[fd].guest) {
        RC_RETURN(c, INVALID_HANDLE, 0);
    }
    if (fd < 3 && files[fd].handle == INVALID_HANDLE) {
        uint32_t a[1] = {(uint32_t)(-10 - fd)};
        files[fd].handle = k32(c, "GetStdHandle", 1, a);
    }
    RC_RETURN(c, files[fd].handle, 0);
}

const rc_import_def rc_msvcrt_stdio[] = {
    {MSVCRT, "fopen", k_fopen},
    {MSVCRT, "fclose", k_fclose},
    {MSVCRT, "fread", k_fread},
    {MSVCRT, "fwrite", k_fwrite},
    {MSVCRT, "fseek", k_fseek},
    {MSVCRT, "ftell", k_ftell},
    {MSVCRT, "fgetpos", k_fgetpos},
    {MSVCRT, "fsetpos", k_fsetpos},
    {MSVCRT, "fflush", k_fflush},
    {MSVCRT, "setvbuf", k_setvbuf},
    {MSVCRT, "feof", k_feof},
    {MSVCRT, "fgetc", k_fgetc},
    {MSVCRT, "getc", k_fgetc},
    {MSVCRT, "ungetc", k_ungetc},
    {MSVCRT, "fputc", k_fputc},
    {MSVCRT, "putc", k_fputc},
    {MSVCRT, "fgets", k_fgets},
    {MSVCRT, "fputs", k_fputs},
    {MSVCRT, "_fstat", k__fstat},
    {MSVCRT, "_get_osfhandle", k__get_osfhandle},
};
const uint32_t rc_msvcrt_stdio_count = sizeof rc_msvcrt_stdio / sizeof rc_msvcrt_stdio[0];

extern const rc_import_def rc_msvcrt_format[];
extern const uint32_t rc_msvcrt_format_count;

void rc_msvcrt_stdio_init(void)
{
    rc_register_imports(rc_msvcrt_stdio, rc_msvcrt_stdio_count);
    rc_register_imports(rc_msvcrt_format, rc_msvcrt_format_count);
    rc_register_data(MSVCRT, "_iob", var_iob);
    rc_register_data(MSVCRT, "__pioinfo", var_pioinfo);
}
