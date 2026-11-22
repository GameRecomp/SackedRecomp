/* SPDX-License-Identifier: MIT */
/* DLS level 1/2 collections (the games' own .dls files): instruments, regions, waves. */
#ifndef RC_DLS_H
#define RC_DLS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct dls_wave {
    int16_t *pcm;       /* mono 16-bit samples */
    uint32_t frames;
    uint32_t rate;
    int unity;          /* MIDI note at which it plays at its own rate */
    int fine;           /* cents */
    float gain;         /* from the attenuation */
    bool looped;
    uint32_t loop_start, loop_len;
} dls_wave;

typedef struct dls_region {
    uint8_t key_lo, key_hi, vel_lo, vel_hi;
    dls_wave wave;      /* the wave with the region's own sample settings applied */
} dls_region;

typedef struct dls_instrument {
    uint32_t bank;      /* DLS ulBank: drum flag, CC0 << 8, CC32 */
    uint32_t program;
    dls_region *regions;
    uint32_t nregions;
    /* Volume envelope, seconds and level (0..1). */
    float attack, decay, sustain, release;
} dls_instrument;

typedef struct dls_collection {
    dls_instrument *instruments;
    uint32_t ninstruments;
} dls_collection;

/* Parse a collection from memory; NULL if it is not one. The data is copied. */
dls_collection *dls_load(const uint8_t *data, uint32_t size);

/* The instrument for a DirectMusic patch (MSB << 16 | LSB << 8 | program, 0x80000000 for
 * drums), or NULL. */
const dls_instrument *dls_find(const dls_collection *c, uint32_t patch);

/* The region of an instrument for a key and velocity, or NULL. */
const dls_region *dls_region_for(const dls_instrument *i, int key, int vel);

#endif
