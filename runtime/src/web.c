/* SPDX-License-Identifier: MIT */
/* The runtime in a browser: see rc/web.h. */
#if defined(__EMSCRIPTEN__)

#include "rc/web.h"

#include <emscripten.h>

static double last; /* when the browser last had its turn */

void rc_web_turn(void) { last = emscripten_get_now(); }

static double turns_ms; /* the time the browser's turns took, in all */

double rc_web_turns_ms(void) { return turns_ms; }

void rc_web_poll(void)
{
    double now = emscripten_get_now();
    if (now - last >= 16.0) {
        rc_web_yield();
        rc_web_turn();
        turns_ms += last - now;
    }
}

#else
/* ISO C wants something in a translation unit. */
typedef int rc_web_unused;
#endif
