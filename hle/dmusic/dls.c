/* SPDX-License-Identifier: MIT */
/*
 * DLS collections: what DirectMusic's synthesizer plays from.
 *
 * A collection holds instruments (bank and program), each with regions (key and velocity
 * ranges) that link to waves in the pool. A region's wave sample chunk (`wsmp`) gives the
 * unity note, fine tune, attenuation and loop, overriding the wave's own. Articulation (`art1`,
 * `art2`) gives the volume envelope; other modulation is not modelled.
 */
#include "dls.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

typedef struct span {
    const uint8_t *p;
    uint32_t n;
} span;

/* The next chunk in [*at, end): id, body; false at the end. */
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

#define ID(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

/* A LIST's type and contents. */
static bool list_of(span body, uint32_t *type, span *inner)
{
    if (body.n < 4) {
        return false;
    }
    *type = rd32(body.p);
    inner->p = body.p + 4;
    inner->n = body.n - 4;
    return true;
}

typedef struct wsmp {
    bool have;
    int unity, fine;
    float gain;
    bool looped;
    uint32_t loop_start, loop_len;
} wsmp;

static wsmp parse_wsmp(span b)
{
    wsmp w = {0};
    if (b.n < 20) {
        return w;
    }
    uint32_t cb = rd32(b.p);
    w.have = true;
    w.unity = rd16(b.p + 4);
    w.fine = (int16_t)rd16(b.p + 6);
    int32_t att = (int32_t)rd32(b.p + 8); /* relative gain units: 1/655360 dB */
    w.gain = powf(10.0f, (float)att / 655360.0f / 20.0f);
    uint32_t loops = rd32(b.p + 16);
    if (loops && cb + 16 <= b.n) {
        const uint8_t *l = b.p + cb;
        w.looped = true;
        w.loop_start = rd32(l + 8);
        w.loop_len = rd32(l + 12);
    }
    return w;
}

typedef struct pool_wave {
    int16_t *pcm;
    uint32_t frames, rate;
    wsmp sm;
    uint32_t offset; /* within the pool list, for the cue table */
} pool_wave;

static void parse_wave(span inner, pool_wave *w)
{
    const uint8_t *at = inner.p, *end = inner.p + inner.n;
    uint32_t id;
    span b;
    uint16_t channels = 1, bits = 16;
    while (next_chunk(&at, end, &id, &b)) {
        if (id == ID('f', 'm', 't', ' ') && b.n >= 16) {
            channels = rd16(b.p + 2);
            w->rate = rd32(b.p + 4);
            bits = rd16(b.p + 14);
        } else if (id == ID('w', 's', 'm', 'p')) {
            w->sm = parse_wsmp(b);
        } else if (id == ID('d', 'a', 't', 'a')) {
            uint32_t frame = (uint32_t)channels * (bits / 8u);
            if (!frame) {
                continue;
            }
            w->frames = b.n / frame;
            w->pcm = malloc((size_t)w->frames * 2 + 2);
            if (!w->pcm) {
                w->frames = 0;
                continue;
            }
            for (uint32_t i = 0; i < w->frames; i++) {
                const uint8_t *s = b.p + (size_t)i * frame;
                int v = bits == 8 ? ((int)s[0] - 128) * 256 : (int16_t)rd16(s);
                w->pcm[i] = (int16_t)v; /* the first channel */
            }
        }
    }
}

/* Volume envelope from connection blocks (DLS 1 art1 / DLS 2 art2). */
static void parse_art(span b, dls_instrument *ins)
{
    if (b.n < 8) {
        return;
    }
    uint32_t cb = rd32(b.p), n = rd32(b.p + 4);
    for (uint32_t i = 0; i < n && cb + 12 * (i + 1) <= b.n; i++) {
        const uint8_t *c = b.p + cb + 12 * i;
        uint16_t src = rd16(c), ctl = rd16(c + 2), dst = rd16(c + 4);
        int32_t scale = (int32_t)rd32(c + 8);
        if (src != 0 || ctl != 0) {
            continue; /* modulated by something: not modelled */
        }
        float secs = scale == (int32_t)0x80000000 ? 0.0f : powf(2.0f, (float)scale / (1200.0f * 65536.0f));
        switch (dst) {
        case 0x206: ins->attack = secs; break;
        case 0x207: ins->decay = secs; break;
        case 0x209: ins->release = secs; break;
        case 0x20a: ins->sustain = (float)scale / 65536.0f / 1000.0f; break;
        default: break;
        }
    }
}

typedef struct raw_region {
    dls_region r;
    wsmp sm;
    uint32_t table_index;
} raw_region;

static void parse_region(span inner, raw_region *rr)
{
    const uint8_t *at = inner.p, *end = inner.p + inner.n;
    uint32_t id;
    span b;
    rr->r.key_hi = 127;
    rr->r.vel_hi = 127;
    while (next_chunk(&at, end, &id, &b)) {
        if (id == ID('r', 'g', 'n', 'h') && b.n >= 8) {
            rr->r.key_lo = (uint8_t)rd16(b.p);
            rr->r.key_hi = (uint8_t)rd16(b.p + 2);
            rr->r.vel_lo = (uint8_t)rd16(b.p + 4);
            rr->r.vel_hi = (uint8_t)rd16(b.p + 6);
            if (rr->r.vel_hi == 0) {
                rr->r.vel_hi = 127;
            }
        } else if (id == ID('w', 's', 'm', 'p')) {
            rr->sm = parse_wsmp(b);
        } else if (id == ID('w', 'l', 'n', 'k') && b.n >= 12) {
            rr->table_index = rd32(b.p + 8);
        }
    }
}

typedef struct raw_ins {
    dls_instrument ins;
    raw_region *regions;
    uint32_t nregions, cap;
} raw_ins;

static void parse_ins(span inner, raw_ins *ri)
{
    const uint8_t *at = inner.p, *end = inner.p + inner.n;
    uint32_t id, type;
    span b, in;
    ri->ins.sustain = 1.0f;
    ri->ins.release = 0.05f;
    while (next_chunk(&at, end, &id, &b)) {
        if (id == ID('i', 'n', 's', 'h') && b.n >= 12) {
            ri->ins.bank = rd32(b.p + 4);
            ri->ins.program = rd32(b.p + 8);
        } else if (id == ID('L', 'I', 'S', 'T') && list_of(b, &type, &in)) {
            if (type == ID('l', 'r', 'g', 'n')) {
                const uint8_t *a2 = in.p, *e2 = in.p + in.n;
                uint32_t id2, t2;
                span b2, in2;
                while (next_chunk(&a2, e2, &id2, &b2)) {
                    if (id2 == ID('L', 'I', 'S', 'T') && list_of(b2, &t2, &in2) &&
                        (t2 == ID('r', 'g', 'n', ' ') || t2 == ID('r', 'g', 'n', '2'))) {
                        if (ri->nregions == ri->cap) {
                            ri->cap = ri->cap ? 2 * ri->cap : 8;
                            ri->regions = realloc(ri->regions, ri->cap * sizeof *ri->regions);
                        }
                        raw_region *rr = &ri->regions[ri->nregions++];
                        memset(rr, 0, sizeof *rr);
                        parse_region(in2, rr);
                    }
                }
            } else if (type == ID('l', 'a', 'r', 't') || type == ID('l', 'a', 'r', '2')) {
                const uint8_t *a2 = in.p, *e2 = in.p + in.n;
                uint32_t id2;
                span b2;
                while (next_chunk(&a2, e2, &id2, &b2)) {
                    if (id2 == ID('a', 'r', 't', '1') || id2 == ID('a', 'r', 't', '2')) {
                        parse_art(b2, &ri->ins);
                    }
                }
            }
        }
    }
}

dls_collection *dls_load(const uint8_t *data, uint32_t size)
{
    if (size < 12 || rd32(data) != ID('R', 'I', 'F', 'F') || rd32(data + 8) != ID('D', 'L', 'S', ' ')) {
        return NULL;
    }
    const uint8_t *at = data + 12, *end = data + (rd32(data + 4) + 8 < size ? rd32(data + 4) + 8 : size);
    uint32_t id, type;
    span b, in;
    raw_ins *ins = NULL;
    uint32_t nins = 0, inscap = 0;
    pool_wave *waves = NULL;
    uint32_t nwaves = 0, wavecap = 0;
    uint32_t *cues = NULL, ncues = 0;
    while (next_chunk(&at, end, &id, &b)) {
        if (id == ID('p', 't', 'b', 'l') && b.n >= 8) {
            uint32_t cb = rd32(b.p);
            ncues = rd32(b.p + 4);
            cues = calloc(ncues ? ncues : 1, 4);
            for (uint32_t i = 0; cues && i < ncues && cb + 4 * (i + 1) <= b.n; i++) {
                cues[i] = rd32(b.p + cb + 4 * i);
            }
        } else if (id == ID('L', 'I', 'S', 'T') && list_of(b, &type, &in)) {
            const uint8_t *a2 = in.p, *e2 = in.p + in.n;
            uint32_t id2, t2;
            span b2, in2;
            while (next_chunk(&a2, e2, &id2, &b2)) {
                if (id2 != ID('L', 'I', 'S', 'T') || !list_of(b2, &t2, &in2)) {
                    continue;
                }
                if (type == ID('l', 'i', 'n', 's') && t2 == ID('i', 'n', 's', ' ')) {
                    if (nins == inscap) {
                        inscap = inscap ? 2 * inscap : 16;
                        ins = realloc(ins, inscap * sizeof *ins);
                    }
                    raw_ins *ri = &ins[nins++];
                    memset(ri, 0, sizeof *ri);
                    parse_ins(in2, ri);
                } else if (type == ID('w', 'v', 'p', 'l') && t2 == ID('w', 'a', 'v', 'e')) {
                    if (nwaves == wavecap) {
                        wavecap = wavecap ? 2 * wavecap : 16;
                        waves = realloc(waves, wavecap * sizeof *waves);
                    }
                    pool_wave *w = &waves[nwaves++];
                    memset(w, 0, sizeof *w);
                    /* Offsets in the cue table count from the start of the pool's list data. */
                    w->offset = (uint32_t)(b2.p - 8 - in.p);
                    parse_wave(in2, w);
                }
            }
        }
    }
    dls_collection *c = calloc(1, sizeof *c);
    if (!c) {
        return NULL;
    }
    c->instruments = calloc(nins ? nins : 1, sizeof *c->instruments);
    c->ninstruments = nins;
    for (uint32_t i = 0; i < nins; i++) {
        dls_instrument *di = &c->instruments[i];
        *di = ins[i].ins;
        di->regions = calloc(ins[i].nregions ? ins[i].nregions : 1, sizeof *di->regions);
        di->nregions = 0;
        for (uint32_t k = 0; k < ins[i].nregions; k++) {
            raw_region *rr = &ins[i].regions[k];
            /* The wave by its cue (offset in the pool), else by its index. */
            pool_wave *w = NULL;
            if (cues && rr->table_index < ncues) {
                for (uint32_t j = 0; j < nwaves && !w; j++) {
                    if (waves[j].offset == cues[rr->table_index]) {
                        w = &waves[j];
                    }
                }
            }
            if (!w && rr->table_index < nwaves) {
                w = &waves[rr->table_index];
            }
            if (!w || !w->pcm) {
                continue;
            }
            dls_region *r = &di->regions[di->nregions++];
            *r = rr->r;
            wsmp sm = rr->sm.have ? rr->sm : w->sm;
            r->wave.pcm = w->pcm; /* shared by regions; owned by the collection */
            r->wave.frames = w->frames;
            r->wave.rate = w->rate ? w->rate : 22050;
            r->wave.unity = sm.have ? sm.unity : 60;
            r->wave.fine = sm.fine;
            r->wave.gain = sm.have ? sm.gain : 1.0f;
            r->wave.looped = sm.looped && sm.loop_len > 0 && sm.loop_start + sm.loop_len <= w->frames;
            r->wave.loop_start = sm.loop_start;
            r->wave.loop_len = sm.loop_len;
        }
        free(ins[i].regions);
    }
    free(ins);
    free(waves); /* the PCM stays with the regions */
    free(cues);
    return c;
}

const dls_instrument *dls_find(const dls_collection *c, uint32_t patch)
{
    if (!c) {
        return NULL;
    }
    uint32_t drum = patch & 0x80000000u;
    uint32_t bank = drum | ((patch >> 16) & 0x7f) << 8 | ((patch >> 8) & 0x7f);
    uint32_t program = patch & 0x7f;
    const dls_instrument *any = NULL;
    for (uint32_t i = 0; i < c->ninstruments; i++) {
        const dls_instrument *in = &c->instruments[i];
        if ((in->program & 0x7f) != program) {
            continue;
        }
        if (in->bank == bank) {
            return in;
        }
        if ((in->bank & 0x80000000u) == drum && !any) {
            any = in;
        }
    }
    return any;
}

const dls_region *dls_region_for(const dls_instrument *in, int key, int vel)
{
    for (uint32_t i = 0; in && i < in->nregions; i++) {
        const dls_region *r = &in->regions[i];
        if (key >= r->key_lo && key <= r->key_hi && vel >= r->vel_lo && vel <= r->vel_hi) {
            return r;
        }
    }
    return NULL;
}
