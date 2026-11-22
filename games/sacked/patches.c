/* SPDX-License-Identifier: MIT */
/*
 * Sacked's patches. The game moves its own pointer by DirectInput's relative mouse motion; in a
 * window that drifts away from the host's pointer (the game starts it in the middle of the
 * screen, puts it back there on some screens and stops it at the edges). With the place where
 * each build keeps the pointer (x, then y, 32 bits each), DirectInput reports the way to the
 * host's pointer instead, so that the two are one (rc_dinput_pointer_at).
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rc/game.h"
#include "rc/hle_modules.h"

void sacked_patches(void);

static const struct {
    const char *variant;
    uint32_t pointer; /* x; y follows */
} builds[] = {
    {"cd-de", 0x0047e30c}, /* Gefeuert.exe */
    {"en", 0x0047e36c},    /* sacked.exe, English and Hungarian */
    {"hu", 0x0047e36c},    /* (the release's name where the converter's code runs) */
    {"pl", 0x0047bf0c},
    {"nl", 0x00478e0c},    /* Ontslagen.exe */
    {"ru", 0x0047e36c},
};

void sacked_patches(void)
{
    const char *variant = rc_game_variant();
    for (size_t i = 0; variant && i < sizeof builds / sizeof builds[0]; i++) {
        if (strcmp(builds[i].variant, variant) == 0) {
            rc_dinput_pointer_at(builds[i].pointer, builds[i].pointer + 4);
        }
    }
}
