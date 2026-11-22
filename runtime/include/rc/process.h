/* SPDX-License-Identifier: MIT */
/* Starting a recompiled program: the guest address space, the image, the
 * main thread, and the call of the entry point. */
#ifndef RC_PROCESS_H
#define RC_PROCESS_H

#include <stdbool.h>
#include <stdint.h>

#include "rc/cpu.h"
#include "rc/dispatch.h"
#include "rc/log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One binary whose lifted code the program contains (see rc_config). */
typedef struct rc_lifted_binary {
    /* File name inside game_dir, e.g. "binkw32.dll". */
    const char *file;
    /* Where it was lifted: 0 for its preferred base; a DLL lifted elsewhere
     * is loaded there with its relocations applied. */
    uint32_t base;
    const rc_fn_entry *table;
    uint32_t count;
} rc_lifted_binary;

typedef struct rc_config {
    /* Short identifier: the prefix of environment switches. */
    const char *id;
    /* Host directory of the installed game (read only). */
    const char *game_dir;
    /* Host directory for files the game writes (saves, settings). */
    const char *user_dir;
    /* The executable inside game_dir. */
    const char *exe;
    /* Where the game believes it is installed, e.g. "C:\\Tropico". */
    const char *guest_dir;
    /* Extra command-line arguments for the guest, already joined. */
    const char *args;
    /* Called after the image is loaded, before the entry point: game
     * projects register their HLE modules and patches here. */
    void (*setup)(void);
    /* Called once the executable and its DLLs are in guest memory, before
     * any of their code runs (the game project's data patches); may be NULL. */
    void (*loaded)(void);
    /* The executable and the DLLs lifted with it, each with its own table
     * (a program that links several builds picks the ones it runs). NULL:
     * the single generated table rc_lifted_table and rc_lifted_modules. */
    const rc_lifted_binary *lifted;
    uint32_t nlifted;
} rc_config;

extern const rc_config *rc_cfg;

/* A crash in guest memory is reported on stderr (the guest address, what
 * kind of place it is, and the last note), then goes on as before.
 * rc_run installs the report; a note says what the program is doing, for
 * example which window message it handles (keep the old one with get and
 * put it back with set when the work nests). */
void rc_fault_install(void);
void rc_fault_note(const char *what, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
const char *rc_fault_note_get(uint32_t v[4]);
void rc_fault_note_set(const char *what, const uint32_t v[4]);

/* The guest command line, module path and similar process data live here. */
typedef struct rc_process {
    uint32_t image_base, image_size, entry;
    uint32_t stack_base, stack_limit;
    uint32_t tib, peb;
    uint32_t cmdline;     /* guest address of the command line */
    uint32_t module_path; /* guest address of the executable's full path */
} rc_process;

extern rc_process rc_proc;

/* A loaded image: the executable or a DLL lifted with it. */
typedef struct rc_module {
    char name[64];
    uint32_t base, size, entry;
    int is_dll;
} rc_module;

/* A loaded module by file name ("binkw32", "BINKW32.DLL"), or NULL. */
const rc_module *rc_module_find(const char *name);
/* The module loaded at `base`, or NULL. */
const rc_module *rc_module_at(uint32_t base);
/* The module whose image holds `addr`, or NULL. */
const rc_module *rc_module_containing(uint32_t addr);
/* The address of an export, by name or "#ordinal", or 0. */
uint32_t rc_module_export(const rc_module *m, const char *name);

/* Run the program; returns its exit code. */
int rc_run(const rc_config *cfg);

/* Leave the process (ExitProcess). */
RC_NORETURN void rc_exit(uint32_t code);

/* A resource of the executable (`module` is its base): guest address and
 * size of its data, by its number or (rc_resource_named) its name; the type
 * is a number. */
bool rc_resource(uint32_t module, uint32_t type, uint32_t id, uint32_t *data, uint32_t *size);
bool rc_resource_named(uint32_t module, uint32_t type, const char *name, uint32_t *data, uint32_t *size);

/* Milliseconds on a monotonic clock (GetTickCount). */
uint64_t rc_ticks_ms(void);
/* Wait (Sleep); with the virtual clock, only advance it. */
void rc_sleep_ms(uint32_t ms);
/* Whether the clock is virtual (RC_CLOCK=virtual): time passes only as
 * the program reads it or sleeps, not with the host's. */
bool rc_clock_virtual(void);
/* Microseconds on the host's monotonic clock, whatever RC_CLOCK says (for
 * measuring the runtime itself). */
uint64_t rc_host_us(void);
/* rc_ticks_ms without advancing the virtual clock. */
uint64_t rc_ticks_peek_ms(void);
/* A frame has been presented. With the virtual clock and RC_CLOCK_FPS=N, it
 * moves on to at least 1/N s after the last frame, as a display at N Hz
 * paces a game, so the game's own timers (a level's clock) run as on a real
 * machine. Without RC_CLOCK_FPS, frames take only the time the program
 * reads. */
void rc_clock_frame(void);
/* RC_CLOCK_FPS from here on (0: frames take only the time read). */
void rc_clock_set_fps(uint32_t fps);

/* A small guest allocation that lives as long as the process. */
uint32_t rc_proc_alloc(uint32_t size);
uint32_t rc_proc_str(const char *s);

#ifdef __cplusplus
}
#endif

#endif
