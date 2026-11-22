/* SPDX-License-Identifier: MIT */
/*
 * MSVCRT.dll, the C runtime Windows ships (Visual C++ 6's), and the bits of MSVCP60.dll the
 * programs use: what Sacked, STLport and the Vorbis DLLs import from it.
 *
 * Files go through kernel32 (the guest's CreateFileA, ReadFile, …), so they see the same files
 * as the rest of the program; threads through CreateThread; C++ exceptions through
 * RaiseException and the guest's registration records (eh.c).
 */
#ifndef RC_HLE_MSVCRT_H
#define RC_HLE_MSVCRT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "rc/hle.h"

#define MSVCRT "MSVCRT.dll"
#define MSVCP60 "MSVCP60.dll"

/* Call guest code from an HLE function and leave the stack as it was (cdecl and stdcall alike);
 * after an exception that continues elsewhere (c->unwind) the stack is the handler's. */
uint32_t rc_crt_call(rc_cpu *c, uint32_t fn, uint32_t n, const uint32_t *args);
/* The same with ECX set (thiscall). */
uint32_t rc_crt_call_this(rc_cpu *c, uint32_t fn, uint32_t self, uint32_t n, const uint32_t *args);
/* Call a function the compiler made from part of another one (a catch block, a destructor
 * call during unwinding, an __except filter): with `ebp` as its frame, the other registers kept. */
uint32_t rc_crt_call_funclet(rc_cpu *c, uint32_t fn, uint32_t ebp);

/* A guest double at `a`. */
double rc_crt_f64(uint32_t a);

/* The C heap (malloc), on the process heap. */
uint32_t rc_crt_malloc(uint32_t n);
void rc_crt_free(uint32_t p);

/* printf-style formatting with the arguments on the guest stack from `args` (as va_list points
 * there); the length the whole result would have, `out` gets at most `cap` bytes with the NUL
 * (cap 0: nothing written). `wide` is not supported. */
uint32_t rc_crt_format(char *out, uint32_t cap, const char *fmt, uint32_t args);
/* sscanf: the number of fields assigned. */
int rc_crt_scan(const char *in, const char *fmt, uint32_t args);

extern const rc_import_def rc_msvcrt_crt[];
extern const uint32_t rc_msvcrt_crt_count;
extern const rc_import_def rc_msvcrt_stdio[];
extern const uint32_t rc_msvcrt_stdio_count;
extern const rc_import_def rc_msvcrt_eh[];
extern const uint32_t rc_msvcrt_eh_count;
extern const rc_import_def rc_msvcp60_defs[];
extern const uint32_t rc_msvcp60_count;

void rc_msvcrt_init(void);
void rc_msvcrt_stdio_init(void);
void rc_msvcrt_eh_init(void);
void rc_msvcp60_string_init(void);

#endif
