/* SPDX-License-Identifier: MIT */
/* A monotonic millisecond clock, as GetTickCount and message times use it. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rc/process.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#if defined(__EMSCRIPTEN__)
#include "rc/web.h"
#endif

static uint64_t host_ms(void)
{
#if defined(_WIN32)
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/* RC_CLOCK=virtual: time advances by a fixed step per reading (100 us, or
 * RC_CLOCK_STEP_US microseconds) and by what Sleep asks for, so a run
 * depends only on what the program does and replays exactly (scripted
 * scenarios use it). The step is small because the game reads the clock
 * about a hundred times a frame: at 1 ms a frame would seem to take 0.1 s,
 * and Bink would drop frames to keep up with the sound. */
static int virtual_clock = -1;
static uint64_t virtual_us = UINT64_C(1000000000);
static uint64_t step_us = 100;
static uint64_t frame_us;      /* RC_CLOCK_FPS: the shortest frame, 0 for none */
static uint64_t last_frame_us; /* when the last frame was presented */

static int is_virtual(void)
{
    if (virtual_clock < 0) {
        const char *v = getenv("RC_CLOCK");
        virtual_clock = v && strcmp(v, "virtual") == 0;
        const char *st = getenv("RC_CLOCK_STEP_US");
        if (st && atoi(st) > 0) {
            step_us = (uint64_t)atoi(st);
        }
        const char *fps = getenv("RC_CLOCK_FPS");
        if (fps && atoi(fps) > 0) {
            frame_us = UINT64_C(1000000) / (uint64_t)atoi(fps);
        }
    }
    return virtual_clock;
}

bool rc_clock_virtual(void) { return is_virtual() != 0; }

void rc_clock_set_fps(uint32_t fps)
{
    is_virtual(); /* (reads RC_CLOCK_FPS first, so this wins) */
    frame_us = fps ? UINT64_C(1000000) / fps : 0;
}

void rc_clock_frame(void)
{
    if (!is_virtual() || !frame_us) {
        return;
    }
    if (virtual_us < last_frame_us + frame_us) {
        virtual_us = last_frame_us + frame_us;
    }
    last_frame_us = virtual_us;
}

void rc_sleep_ms(uint32_t ms)
{
    if (is_virtual()) {
        virtual_us += (uint64_t)ms * 1000u;
        return;
    }
#if defined(_WIN32)
    Sleep(ms);
#elif defined(__EMSCRIPTEN__)
    /* The browser runs meanwhile; Sleep(0) gives it a turn when due. */
    if (ms == 0) {
        rc_web_poll();
    } else {
        rc_web_sleep(ms);
        rc_web_turn();
    }
#else
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

uint64_t rc_host_us(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart * 1e6 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

uint64_t rc_host_turns_us(void)
{
#if defined(__EMSCRIPTEN__)
    return (uint64_t)(rc_web_turns_ms() * 1000.0);
#else
    return 0;
#endif
}

uint64_t rc_ticks_peek_ms(void)
{
    if (is_virtual()) {
        return virtual_us / 1000u;
    }
    return rc_ticks_ms();
}

uint64_t rc_ticks_ms(void)
{
    if (is_virtual()) {
        uint64_t ms = virtual_us / 1000u;
        virtual_us += step_us;
        return ms;
    }
    /* As if the machine had been up for a while: some code of the time
     * treats small tick counts specially. */
    static uint64_t start;
    if (!start) {
        start = host_ms() - 1000000u;
    }
    return host_ms() - start;
}
