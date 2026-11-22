/* SPDX-License-Identifier: MIT */
/* The runtime in a browser: see rc/web.h. */
#if defined(__EMSCRIPTEN__)

#include "rc/web.h"

#include <emscripten.h>

static double last; /* when the browser last had its turn */

void rc_web_turn(void) { last = emscripten_get_now(); }

void rc_web_poll(void)
{
    double now = emscripten_get_now();
    if (now - last >= 16.0) {
        rc_web_yield();
        rc_web_turn();
    }
}

#else
/* ISO C wants something in a translation unit. */
typedef int rc_web_unused;
#endif
