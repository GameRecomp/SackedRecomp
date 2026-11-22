/* SPDX-License-Identifier: MIT */
/* DirectMusic segments played through a small DLS synthesizer (player.c). */
#ifndef RC_DM_PLAYER_H
#define RC_DM_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct dm_segment dm_segment;

/* Collections a band refers to by file name are looked up through this (the loader's search
 * directory): it returns the file's bytes (malloc'd) or NULL. */
typedef uint8_t *(*dm_reader)(const char *file, uint32_t *size);

/* Parse a segment file (RIFF DMSG); NULL if it is not one. */
dm_segment *dm_segment_load(const uint8_t *data, uint32_t size, dm_reader read);
/* Length in music time (768 ticks a quarter note) and repeats (0xffffffff: forever). */
uint32_t dm_segment_length(const dm_segment *s);
uint32_t dm_segment_repeats(const dm_segment *s);
void dm_segment_set_repeats(dm_segment *s, uint32_t repeats);

/* Play a segment now: a primary one replaces the primary playing; secondary ones play over it. */
void dm_play(dm_segment *s, bool secondary);
/* Stop a segment (NULL: everything). */
void dm_stop(const dm_segment *s);
bool dm_is_playing(const dm_segment *s);
/* The music time since the primary segment started. */
uint32_t dm_music_time(void);
/* Master volume in hundredths of a decibel (0 full, -10000 silent). */
void dm_set_volume(int32_t hundredths_db);
/* Notes played, the ones no instrument region covered, and the loudest sample (traces). */
extern uint32_t dm_notes, dm_missed;
extern float dm_peak;
/* Render what is due (called at every message pump). */
void dm_pump(void);

#endif
