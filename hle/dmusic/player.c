/* SPDX-License-Identifier: MIT */
/*
 * DirectMusic segments, played through a small DLS synthesizer into a platform PCM stream.
 *
 * A segment (RIFF DMSG) becomes a sorted list of events in music time (768 ticks a quarter
 * note), from its tracks:
 *   - tempo (`tetr`): ticks to samples;
 *   - band (`DMBT`): per performance channel, an instrument of a DLS collection the band names
 *     by file (the games' own .dls files), with volume, pan and transpose;
 *   - pattern (`DMPT`): notes on a grid, per part and variation, repeated over the segment's
 *     length; a variation is picked at random for each repetition, as DirectMusic does by
 *     default. The games' patterns use the fixed play mode only: a note's music value is its
 *     MIDI key (chord and scale mapping are not modelled);
 *   - sequence (`seqt`): MIDI notes and controllers (volume, expression, pan).
 * Chord, time-signature and other tracks change nothing here.
 *
 * The synthesizer plays each note from the region of the instrument for its key and velocity:
 * the wave resampled from its unity note, looped where it has a loop, with the instrument's
 * attack, decay, sustain and release, velocity and channel volume squared, and pan.
 */
#include "player.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dls.h"
#include "rc/audio.h"

#define PPQ 768u
#define RATE 44100
#define CHANNELS 128
#define VOICES 64
#define PLAYING 8

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
#define ID(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

typedef struct span {
    const uint8_t *p;
    uint32_t n;
} span;

static bool next_chunk(const uint8_t **at, const uint8_t *end, uint32_t *id, span *body)
{
    if (end - *at < 8) {
        return false;
    }
    *id = rd32(*at);
    uint32_t n = rd32(*at + 4);
    if (n > (uint32_t)(end - *at - 8)) {
        n = (uint32_t)(end - *at - 8);
    }
    body->p = *at + 8;
    body->n = n;
    *at += 8 + n + (n & 1);
    return true;
}

/* ---- events ---- */

enum { EV_NOTE_ON, EV_NOTE_OFF, EV_INSTRUMENT, EV_VOLUME, EV_PAN, EV_EXPRESSION };

typedef struct event {
    uint32_t tick;
    uint8_t type;
    uint8_t key, vel;
    uint8_t channel;
    const dls_instrument *ins; /* EV_INSTRUMENT */
    int8_t transpose;
    uint8_t value;             /* volume, pan, expression; EV_INSTRUMENT: volume (128: unchanged) */
    uint8_t pan;               /* EV_INSTRUMENT: pan (128: unchanged) */
} event;

typedef struct tempo {
    uint32_t tick;
    double bpm;
} tempo;

struct dm_segment {
    event *ev;
    uint32_t nev, cap;
    tempo *tempos;
    uint32_t ntempos;
    uint32_t length, repeats, loop_start, loop_end;
};

static void add(dm_segment *s, event e)
{
    if (s->nev == s->cap) {
        s->cap = s->cap ? 2 * s->cap : 256;
        s->ev = realloc(s->ev, s->cap * sizeof *s->ev);
    }
    s->ev[s->nev++] = e;
}

static int by_tick(const void *a, const void *b)
{
    const event *x = a, *y = b;
    if (x->tick != y->tick) {
        return x->tick < y->tick ? -1 : 1;
    }
    /* At the same time: instruments, then note offs, then the rest. */
    int ox = x->type == EV_INSTRUMENT ? 0 : x->type == EV_NOTE_OFF ? 1 : 2;
    int oy = y->type == EV_INSTRUMENT ? 0 : y->type == EV_NOTE_OFF ? 1 : 2;
    return ox - oy;
}

/* ---- collections a band refers to ---- */

#define COLLECTIONS 64
static struct {
    char file[64];
    dls_collection *c;
} collections[COLLECTIONS];

static dls_collection *collection(const char *file, dm_reader read)
{
    for (int i = 0; i < COLLECTIONS && collections[i].file[0]; i++) {
        if (strcmp(collections[i].file, file) == 0) {
            return collections[i].c;
        }
    }
    uint32_t size = 0;
    uint8_t *data = read ? read(file, &size) : NULL;
    dls_collection *c = data ? dls_load(data, size) : NULL;
    free(data);
    for (int i = 0; i < COLLECTIONS; i++) {
        if (!collections[i].file[0]) {
            snprintf(collections[i].file, sizeof collections[i].file, "%s", file);
            collections[i].c = c;
            break;
        }
    }
    return c;
}

/* A wide (UTF-16) string as ASCII. */
static void ascii(span b, char *out, size_t cap)
{
    size_t n = 0;
    for (uint32_t i = 0; i + 1 < b.n && n + 1 < cap; i += 2) {
        uint16_t ch = rd16(b.p + i);
        if (!ch) {
            break;
        }
        out[n++] = ch < 0x80 ? (char)ch : '_';
    }
    out[n] = 0;
}

/* ---- tracks ---- */

static void band_instrument(dm_segment *s, span inner, uint32_t time, dm_reader read)
{
    const uint8_t *at = inner.p, *end = inner.p + inner.n;
    uint32_t id;
    span b;
    const uint8_t *bins = NULL;
    uint32_t bins_n = 0;
    char file[64] = "";
    while (next_chunk(&at, end, &id, &b)) {
        if (id == ID('b', 'i', 'n', 's')) {
            bins = b.p;
            bins_n = b.n;
        } else if (id == ID('L', 'I', 'S', 'T') && b.n >= 4 && rd32(b.p) == ID('D', 'M', 'R', 'F')) {
            const uint8_t *a2 = b.p + 4, *e2 = b.p + b.n;
            uint32_t id2;
            span b2;
            while (next_chunk(&a2, e2, &id2, &b2)) {
                if (id2 == ID('f', 'i', 'l', 'e')) {
                    ascii(b2, file, sizeof file);
                }
            }
        }
    }
    if (!bins || bins_n < 36) {
        return;
    }
    uint32_t patch = rd32(bins), pch = rd32(bins + 24), flags = rd32(bins + 28);
    event e = {.tick = time, .type = EV_INSTRUMENT, .channel = (uint8_t)(pch % CHANNELS), .value = 128, .pan = 128};
    if ((flags & 0x1) && file[0]) { /* DMUS_IO_INST_PATCH */
        const char *base = strrchr(file, '\\');
        e.ins = dls_find(collection(base ? base + 1 : file, read), patch);
    }
    if (flags & 0x40) { /* VOLUME */
        e.value = bins[33];
    }
    if (flags & 0x20) { /* PAN */
        e.pan = bins[32];
    }
    if (flags & 0x80) { /* TRANSPOSE */
        e.transpose = (int8_t)(int16_t)rd16(bins + 34);
    }
    add(s, e);
}

static void band_track(dm_segment *s, span inner, dm_reader read);

/* Walk RIFF/LIST structure for bands: LIST lbnd (bdih/bd2h, RIFF DMBD > LIST lbil > LIST lbin). */
static void bands(dm_segment *s, span inner, uint32_t time, dm_reader read)
{
    const uint8_t *at = inner.p, *end = inner.p + inner.n;
    uint32_t id;
    span b;
    while (next_chunk(&at, end, &id, &b)) {
        if ((id == ID('b', 'd', 'i', 'h') || id == ID('b', 'd', '2', 'h')) && b.n >= 4) {
            time = rd32(b.p);
        } else if ((id == ID('L', 'I', 'S', 'T') || id == ID('R', 'I', 'F', 'F')) && b.n >= 4) {
            uint32_t type = rd32(b.p);
            span in = {b.p + 4, b.n - 4};
            if (type == ID('l', 'b', 'i', 'n')) {
                band_instrument(s, in, time, read);
            } else {
                bands(s, in, time, read);
            }
        }
    }
}

static void band_track(dm_segment *s, span inner, dm_reader read) { bands(s, inner, 0, read); }

static void sequence_track(dm_segment *s, span inner)
{
    const uint8_t *at = inner.p, *end = inner.p + inner.n;
    uint32_t id;
    span b;
    while (next_chunk(&at, end, &id, &b)) {
        if (id != ID('e', 'v', 't', 'l') || b.n < 4) {
            continue;
        }
        uint32_t size = rd32(b.p);
        if (size < 17) {
            continue;
        }
        for (uint32_t o = 4; o + size <= b.n; o += size) {
            const uint8_t *e = b.p + o;
            uint32_t t = rd32(e) + (uint32_t)(int16_t)rd16(e + 12), dur = rd32(e + 4);
            uint8_t ch = (uint8_t)(rd32(e + 8) % CHANNELS), st = e[14] & 0xf0, d1 = e[15], d2 = e[16];
            if (st == 0x90 && d2) {
                add(s, (event){.tick = t, .type = EV_NOTE_ON, .key = d1, .vel = d2, .channel = ch});
                add(s, (event){.tick = t + (dur ? dur : 1), .type = EV_NOTE_OFF, .key = d1, .channel = ch});
            } else if (st == 0xb0 && (d1 == 7 || d1 == 10 || d1 == 11)) {
                uint8_t type = d1 == 7 ? EV_VOLUME : d1 == 10 ? EV_PAN : EV_EXPRESSION;
                add(s, (event){.tick = t, .type = type, .channel = ch, .value = d2});
            }
        }
    }
}

/* The default chord DirectMusic uses without a chord track: C major 7 (C E G B) on the C
 * major scale. A music value of octave o lands in MIDI octave o - 1: measured on the games'
 * own segments against the key ranges of their DLS instruments (12 % of notes outside the
 * instruments' regions, against 40 % an octave higher; the drum parts fit the kit's keys only
 * so). */
static const int chord_root = -12;
static const uint32_t chord_pattern = 0x891, scale_pattern = 0xab5;

/* A pattern note's music value as a MIDI key (DirectMusic's MusicToMIDI for one chord). */
static int music_to_midi(uint16_t v, uint8_t mode)
{
    if (mode == 0 || mode == 1) { /* fixed, fixed to the key (C) */
        return v;
    }
    int octave = (v >> 12) & 0xf, chord_pos = (v >> 8) & 0xf, scale_pos = (v >> 4) & 0xf, acc = v & 0xf;
    if (acc > 7) {
        acc -= 16;
    }
    int tones[24], n = 0;
    for (int i = 0; i < 24; i++) {
        if (chord_pattern >> i & 1) {
            tones[n++] = i;
        }
    }
    int note = 12 * octave + ((mode & 0x2) ? chord_root : 0);
    if (mode & 0x8) { /* chord intervals */
        note += tones[chord_pos % n] + 12 * (chord_pos / n);
    }
    if (mode & 0x4) { /* scale intervals: steps up the scale */
        for (int k = 0; k < scale_pos; k++) {
            note++;
            while (!(scale_pattern >> (((note - chord_root) % 12 + 12) % 12) & 1)) {
                note++;
            }
        }
    }
    return note + acc;
}

typedef struct part {
    const uint8_t *guid;
    uint8_t mode;
    uint8_t beats, beat;
    uint16_t grids;
    uint16_t measures;
    span notes;
} part;

static uint32_t rng_state = 0x2545f491u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void pattern(dm_segment *s, span inner)
{
    const uint8_t *at = inner.p, *end = inner.p + inner.n;
    uint32_t id;
    span b;
    part parts[32];
    int nparts = 0;
    struct {
        const uint8_t *guid;
        uint32_t pchannel;
    } refs[32];
    int nrefs = 0;
    uint8_t beats = 4, beat = 4;
    uint16_t grids = 4, measures = 1;
    while (next_chunk(&at, end, &id, &b)) {
        if (id == ID('p', 't', 'n', 'h') && b.n >= 10) {
            beats = b.p[0];
            beat = b.p[1];
            grids = rd16(b.p + 2);
            measures = rd16(b.p + 8);
        } else if (id == ID('L', 'I', 'S', 'T') && b.n >= 4) {
            uint32_t type = rd32(b.p);
            const uint8_t *a2 = b.p + 4, *e2 = b.p + b.n;
            uint32_t id2;
            span b2;
            if (type == ID('p', 'a', 'r', 't') && nparts < 32) {
                part *p = &parts[nparts];
                memset(p, 0, sizeof *p);
                while (next_chunk(&a2, e2, &id2, &b2)) {
                    if (id2 == ID('p', 'r', 't', 'h') && b2.n >= 150) {
                        p->beats = b2.p[0];
                        p->beat = b2.p[1];
                        p->grids = rd16(b2.p + 2);
                        p->guid = b2.p + 132;
                        p->measures = rd16(b2.p + 148);
                        p->mode = b2.p[150];
                    } else if (id2 == ID('n', 'o', 't', 'e')) {
                        p->notes = b2;
                    }
                }
                if (p->guid) {
                    nparts++;
                }
            } else if (type == ID('p', 'r', 'e', 'f') && nrefs < 32) {
                while (next_chunk(&a2, e2, &id2, &b2)) {
                    if (id2 == ID('p', 'r', 'f', 'c') && b2.n >= 28) {
                        refs[nrefs].guid = b2.p;
                        refs[nrefs].pchannel = rd32(b2.p + 24);
                        nrefs++;
                    }
                }
            }
        }
    }
    for (int r = 0; r < nrefs; r++) {
        part *p = NULL;
        for (int k = 0; k < nparts && !p; k++) {
            if (memcmp(parts[k].guid, refs[r].guid, 16) == 0) {
                p = &parts[k];
            }
        }
        if (!p || p->notes.n < 4) {
            continue;
        }
        uint8_t pb = p->beats ? p->beats : beats, pbt = p->beat ? p->beat : beat;
        uint16_t pg = p->grids ? p->grids : grids, pm = p->measures ? p->measures : measures;
        uint32_t beat_ticks = PPQ * 4 / (pbt ? pbt : 4);
        uint32_t len = (uint32_t)pm * pb * beat_ticks;
        uint32_t size = rd32(p->notes.p);
        if (!len || size < 20) {
            continue;
        }
        /* The variations that have notes. */
        uint32_t used = 0;
        for (uint32_t o = 4; o + size <= p->notes.n; o += size) {
            used |= rd32(p->notes.p + o + 4);
        }
        if (!used) {
            continue;
        }
        uint8_t ch = (uint8_t)(refs[r].pchannel % CHANNELS);
        for (uint32_t start = 0; start < s->length; start += len) {
            /* A variation at random among those used. */
            int choices[32], n = 0;
            for (int v = 0; v < 32; v++) {
                if (used & (1u << v)) {
                    choices[n++] = v;
                }
            }
            uint32_t var = 1u << choices[rnd() % (uint32_t)n];
            for (uint32_t o = 4; o + size <= p->notes.n; o += size) {
                const uint8_t *e = p->notes.p + o;
                if (!(rd32(e + 4) & var)) {
                    continue;
                }
                uint32_t grid = rd32(e), dur = rd32(e + 8);
                int16_t offset = (int16_t)rd16(e + 12);
                uint8_t mode = size > 21 && e[21] != 16 ? e[21] : p->mode;
                int midi = music_to_midi(rd16(e + 14), mode);
                uint8_t vel = e[16];
                uint32_t g = pg ? pg : 4;
                int64_t t = (int64_t)start + (int64_t)(grid / g) * beat_ticks + (int64_t)(grid % g) * beat_ticks / g + offset;
                if (t < 0 || (uint32_t)t >= s->length || midi < 0 || midi > 127) {
                    continue;
                }
                add(s, (event){.tick = (uint32_t)t, .type = EV_NOTE_ON, .key = (uint8_t)midi, .vel = vel, .channel = ch});
                add(s, (event){.tick = (uint32_t)t + (dur ? dur : 1), .type = EV_NOTE_OFF, .key = (uint8_t)midi, .channel = ch});
            }
        }
    }
}

static void tracks(dm_segment *s, span inner, dm_reader read)
{
    const uint8_t *at = inner.p, *end = inner.p + inner.n;
    uint32_t id;
    span b;
    while (next_chunk(&at, end, &id, &b)) {
        if (id == ID('t', 'e', 't', 'r') && b.n >= 4) {
            uint32_t size = rd32(b.p);
            for (uint32_t o = 4; size >= 12 && o + size <= b.n; o += size) {
                double bpm;
                memcpy(&bpm, b.p + o + 4, 8);
                if (bpm > 0) {
                    s->tempos = realloc(s->tempos, (s->ntempos + 1) * sizeof *s->tempos);
                    s->tempos[s->ntempos++] = (tempo){rd32(b.p + o), bpm};
                }
            }
        } else if (id == ID('s', 'e', 'q', 't')) {
            sequence_track(s, b);
        } else if ((id == ID('R', 'I', 'F', 'F') || id == ID('L', 'I', 'S', 'T')) && b.n >= 4) {
            uint32_t type = rd32(b.p);
            span in = {b.p + 4, b.n - 4};
            if (type == ID('D', 'M', 'B', 'T')) {
                band_track(s, in, read);
            } else if (type == ID('p', 't', 't', 'n')) {
                pattern(s, in);
            } else {
                tracks(s, in, read);
            }
        }
    }
}

dm_segment *dm_segment_load(const uint8_t *data, uint32_t size, dm_reader read)
{
    if (size < 12 || rd32(data) != ID('R', 'I', 'F', 'F') || rd32(data + 8) != ID('D', 'M', 'S', 'G')) {
        return NULL;
    }
    dm_segment *s = calloc(1, sizeof *s);
    if (!s) {
        return NULL;
    }
    const uint8_t *at = data + 12, *end = data + size;
    uint32_t id;
    span b;
    while (next_chunk(&at, end, &id, &b)) {
        if (id == ID('s', 'e', 'g', 'h') && b.n >= 20) {
            s->repeats = rd32(b.p);
            s->length = rd32(b.p + 4);
            s->loop_start = rd32(b.p + 12);
            s->loop_end = rd32(b.p + 16);
        }
    }
    if (!s->length) {
        s->length = 4 * PPQ;
    }
    at = data + 12;
    while (next_chunk(&at, end, &id, &b)) {
        if (id == ID('L', 'I', 'S', 'T') && b.n >= 4 && rd32(b.p) == ID('t', 'r', 'k', 'l')) {
            tracks(s, (span){b.p + 4, b.n - 4}, read);
        }
    }
    qsort(s->ev, s->nev, sizeof *s->ev, by_tick);
    return s;
}

uint32_t dm_segment_length(const dm_segment *s) { return s->length; }
uint32_t dm_segment_repeats(const dm_segment *s) { return s->repeats; }
void dm_segment_set_repeats(dm_segment *s, uint32_t repeats) { s->repeats = repeats; }

/* ---- the synthesizer ---- */

typedef struct channel {
    const dls_instrument *ins;
    int transpose;
    uint8_t volume, pan, expression;
} channel;

typedef struct voice {
    bool on, released;
    const dls_region *r;
    const dls_instrument *ins;
    uint8_t channel, key;
    const dm_segment *owner;
    double pos, step;
    float gain, env;
    int stage; /* 0 attack, 1 decay, 2 sustain, 3 release */
} voice;

typedef struct play {
    dm_segment *s;
    bool secondary;
    double tick;        /* music time within the segment */
    uint32_t next;      /* next event */
    uint32_t repeats_left;
} play;

static channel channels[CHANNELS];
static voice voices[VOICES];
static play playing[PLAYING];
static int nplaying;
static rc_pcm *out;
static float master = 1.0f;
static uint64_t primary_samples;

static double bpm_at(const dm_segment *s, double tick)
{
    double bpm = 120.0;
    for (uint32_t i = 0; i < s->ntempos && s->tempos[i].tick <= tick; i++) {
        bpm = s->tempos[i].bpm;
    }
    return bpm;
}

static void channels_reset(void)
{
    for (int i = 0; i < CHANNELS; i++) {
        channels[i] = (channel){.volume = 100, .pan = 64, .expression = 127};
    }
}

uint32_t dm_notes, dm_missed;
float dm_peak;

static void note_on(const dm_segment *s, const event *e)
{
    channel *ch = &channels[e->channel];
    int key = e->key + ch->transpose;
    if (!ch->ins || key < 0 || key > 127) {
        return;
    }
    const dls_region *r = dls_region_for(ch->ins, key, e->vel);
    dm_notes++;
    if (!r || !r->wave.frames) {
        dm_missed++;
        return;
    }
    voice *v = NULL;
    for (int i = 0; i < VOICES && !v; i++) {
        if (!voices[i].on) {
            v = &voices[i];
        }
    }
    if (!v) { /* steal a released one, else the first */
        for (int i = 0; i < VOICES && !v; i++) {
            if (voices[i].released) {
                v = &voices[i];
            }
        }
        v = v ? v : &voices[0];
    }
    double semis = (double)(key - r->wave.unity) + r->wave.fine / 100.0;
    *v = (voice){.on = true, .r = r, .ins = ch->ins, .channel = e->channel, .key = e->key, .owner = s,
                 .step = pow(2.0, semis / 12.0) * r->wave.rate / RATE};
    float vel = (float)e->vel / 127.0f;
    v->gain = vel * vel * r->wave.gain;
    v->env = ch->ins->attack > 0.001f ? 0.0f : 1.0f;
    v->stage = v->env < 1.0f ? 0 : 1;
}

static void note_off(const dm_segment *s, const event *e)
{
    for (int i = 0; i < VOICES; i++) {
        voice *v = &voices[i];
        if (v->on && !v->released && v->owner == s && v->channel == e->channel && v->key == e->key) {
            v->released = true;
            v->stage = 3;
        }
    }
}

static void apply(const dm_segment *s, const event *e)
{
    channel *ch = &channels[e->channel];
    switch (e->type) {
    case EV_NOTE_ON: note_on(s, e); break;
    case EV_NOTE_OFF: note_off(s, e); break;
    case EV_INSTRUMENT:
        if (e->ins) {
            ch->ins = e->ins;
        }
        ch->transpose = e->transpose;
        if (e->value <= 127) {
            ch->volume = e->value;
        }
        if (e->pan <= 127) {
            ch->pan = e->pan;
        }
        break;
    case EV_VOLUME: ch->volume = e->value; break;
    case EV_PAN: ch->pan = e->value; break;
    case EV_EXPRESSION: ch->expression = e->value; break;
    default: break;
    }
}

/* Advance a playing segment by `n` samples, firing its events. False when it has ended. */
static bool advance_play(play *p, uint32_t n)
{
    dm_segment *s = p->s;
    p->tick += (double)n * bpm_at(s, p->tick) * PPQ / 60.0 / RATE;
    for (;;) {
        while (p->next < s->nev && s->ev[p->next].tick <= p->tick) {
            apply(s, &s->ev[p->next++]);
        }
        uint32_t loop_end = s->loop_end ? s->loop_end : s->length;
        if (p->tick < loop_end) {
            return true;
        }
        if (p->repeats_left == 0) {
            return p->tick < s->length;
        }
        if (p->repeats_left != 0xffffffffu) {
            p->repeats_left--;
        }
        uint32_t loop_start = s->loop_end ? s->loop_start : 0;
        p->tick -= loop_end - loop_start;
        p->next = 0;
        while (p->next < s->nev && s->ev[p->next].tick < loop_start) {
            p->next++;
        }
    }
}

static void render(int16_t *buf, uint32_t frames)
{
    static float mix[2 * 256];
    memset(mix, 0, sizeof(float) * 2 * frames);
    for (int i = 0; i < VOICES; i++) {
        voice *v = &voices[i];
        if (!v->on) {
            continue;
        }
        const dls_wave *w = &v->r->wave;
        const channel *ch = &channels[v->channel];
        float cv = (float)ch->volume / 127.0f, ex = (float)ch->expression / 127.0f;
        float g = v->gain * cv * cv * ex * ex * master;
        float pan = (float)ch->pan / 127.0f;
        float gl = g * cosf(pan * 1.5707963f), gr = g * sinf(pan * 1.5707963f);
        const dls_instrument *in = v->ins;
        float per = 1.0f / RATE;
        for (uint32_t f = 0; f < frames && v->on; f++) {
            /* Envelope */
            switch (v->stage) {
            case 0:
                v->env += per / (in->attack > 0.001f ? in->attack : 0.001f);
                if (v->env >= 1.0f) {
                    v->env = 1.0f;
                    v->stage = 1;
                }
                break;
            case 1:
                if (in->decay > 0.001f && v->env > in->sustain) {
                    v->env -= per / in->decay;
                }
                if (v->env <= in->sustain || in->decay <= 0.001f) {
                    v->env = in->sustain > 0 ? (in->sustain < 1 ? in->sustain : 1) : v->env;
                    v->stage = 2;
                }
                break;
            case 3:
                v->env -= per / (in->release > 0.005f ? in->release : 0.005f);
                if (v->env <= 0) {
                    v->on = false;
                    continue;
                }
                break;
            default: break;
            }
            uint32_t ip = (uint32_t)v->pos;
            if (ip + 1 >= w->frames) {
                if (!w->looped) {
                    v->on = false;
                    break;
                }
            }
            float frac = (float)(v->pos - ip);
            float a = w->pcm[ip], b = ip + 1 < w->frames ? w->pcm[ip + 1] : a;
            float smp = (a + (b - a) * frac) * v->env;
            mix[2 * f] += smp * gl;
            mix[2 * f + 1] += smp * gr;
            v->pos += v->step;
            if (w->looped && v->pos >= w->loop_start + w->loop_len) {
                v->pos -= w->loop_len;
            }
        }
    }
    for (uint32_t i = 0; i < 2 * frames; i++) {
        float s = mix[i] * 0.5f;
        if (fabsf(s) > dm_peak) {
            dm_peak = fabsf(s);
        }
        buf[i] = (int16_t)(s > 32767.0f ? 32767 : s < -32768.0f ? -32768 : s);
    }
}

void dm_pump(void)
{
    if (!out || !nplaying) {
        return;
    }
    /* About 120 ms queued, in steps of 256 frames. */
    while (rc_pcm_queued(out) < RATE * 4 * 120 / 1000) {
        int16_t buf[2 * 256];
        for (int i = 0; i < nplaying;) {
            if (advance_play(&playing[i], 256)) {
                i++;
            } else {
                dm_stop(playing[i].s);
            }
        }
        primary_samples += 256;
        render(buf, 256);
        rc_pcm_put(out, buf, sizeof buf);
        if (!nplaying) {
            break;
        }
    }
}

void dm_play(dm_segment *s, bool secondary)
{
    if (!s) {
        return;
    }
    if (!out) {
        out = rc_audio_open(RATE, 2) ? rc_pcm_open(RATE, 2, 16) : NULL;
        channels_reset();
    }
    if (!secondary) {
        for (int i = 0; i < nplaying;) {
            if (!playing[i].secondary) {
                dm_stop(playing[i].s);
            } else {
                i++;
            }
        }
        primary_samples = 0;
    }
    dm_stop(s);
    if (nplaying == PLAYING) {
        dm_stop(playing[0].s);
    }
    playing[nplaying++] = (play){.s = s, .secondary = secondary, .repeats_left = s->repeats};
    /* Events at time 0 (bands) at once. */
    advance_play(&playing[nplaying - 1], 0);
}

void dm_stop(const dm_segment *s)
{
    for (int i = 0; i < nplaying;) {
        if (!s || playing[i].s == s) {
            for (int k = 0; k < VOICES; k++) {
                if (voices[k].on && voices[k].owner == playing[i].s) {
                    voices[k].released = true;
                    voices[k].stage = 3;
                }
            }
            playing[i] = playing[--nplaying];
        } else {
            i++;
        }
    }
}

bool dm_is_playing(const dm_segment *s)
{
    for (int i = 0; i < nplaying; i++) {
        if (!s || playing[i].s == s) {
            return true;
        }
    }
    return false;
}

uint32_t dm_music_time(void) { return (uint32_t)(primary_samples * 120.0 * PPQ / 60.0 / RATE); }

void dm_set_volume(int32_t hundredths_db)
{
    master = hundredths_db <= -10000 ? 0.0f : powf(10.0f, (float)hundredths_db / 2000.0f);
}
