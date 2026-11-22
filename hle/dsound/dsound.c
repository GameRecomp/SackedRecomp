/* SPDX-License-Identifier: MIT */
/*
 * DirectSound and DirectSound 8 on the platform's PCM streams.
 *
 * A secondary buffer is guest memory the program locks and fills, with a format, volume, pan
 * and frequency. While it plays, its bytes are fed into a platform stream (rc_pcm) from the
 * play position on: at every message pump and whenever the program asks where playback is,
 * keeping about 80 ms queued. The play cursor is the feed position less what is still queued,
 * so a program that streams into a looping buffer (writing behind the play cursor) is heard
 * as it wrote it. The primary buffer only takes a format; 3D buffers are plain buffers.
 *
 * COM objects are as in ddraw.c: a guest block with the vtable and the host object's index;
 * vtables hold reserved import addresses named after the methods.
 */
#include "dsound.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rc/audio.h"
#include "rc/log.h"
#include "rc/process.h"
#include "rc/vm.h"
#include "user32/user32.h"

#define DS "DSOUND.dll"
#define COM "DSOUND_COM"
#define DS_OK 0u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_NODRIVER 0x88780078u
#define E_NOINTERFACE 0x80004002u

#define DSBCAPS_PRIMARYBUFFER 0x1u
#define DSBPLAY_LOOPING 0x1u
#define DSBSTATUS_PLAYING 0x1u
#define DSBSTATUS_LOOPING 0x4u
#define DSBLOCK_FROMWRITECURSOR 0x1u
#define DSBLOCK_ENTIREBUFFER 0x2u

enum kind { K_FREE, K_DS, K_BUFFER, K_NOTIFY };

typedef struct object {
    enum kind kind;
    uint32_t refs;
    uint32_t guest;
    /* A buffer. */
    bool primary;
    uint32_t data, size;       /* guest memory; shared by duplicates */
    uint32_t rate, channels, bits, align;
    uint32_t freq;             /* playback rate (0: the format's) */
    int32_t volume;            /* hundredths of a decibel, 0 to -10000 */
    int32_t pan;
    bool playing, looping;
    uint32_t feed;             /* the next byte to feed */
    rc_pcm *pcm;
    uint32_t pcm_rate;
    /* With the virtual clock (RC_CLOCK=virtual), how much has played follows that clock, not
     * the audio device, so that a run repeats exactly: bytes put since `vstart` (ms). */
    uint64_t vstart;
    uint32_t vput;
} object;

/* A buffer per sample: the C2-engine games create well over a thousand. */
#define OBJECTS 8192
static object objects[OBJECTS];
static bool audio_ok, hooked;

static const char *const ds_methods[] = {
    "QueryInterface", "AddRef", "Release", "CreateSoundBuffer", "GetCaps", "DuplicateSoundBuffer",
    "SetCooperativeLevel", "Compact", "GetSpeakerConfig", "SetSpeakerConfig", "Initialize",
    "VerifyCertification",
};
static const char *const buffer_methods[] = {
    "QueryInterface", "AddRef", "Release", "GetCaps", "GetCurrentPosition", "GetFormat", "GetVolume",
    "GetPan", "GetFrequency", "GetStatus", "Initialize", "Lock", "Play", "SetCurrentPosition",
    "SetFormat", "SetVolume", "SetPan", "SetFrequency", "Stop", "Unlock", "Restore", "SetFX",
    "AcquireResources", "GetObjectInPath",
};
static const char *const notify_methods[] = {"QueryInterface", "AddRef", "Release", "SetNotificationPositions"};

static uint32_t vtable(enum kind k)
{
    static uint32_t tables[4];
    if (tables[k]) {
        return tables[k];
    }
    const char *const *m = k == K_DS ? ds_methods : k == K_BUFFER ? buffer_methods : notify_methods;
    const char *name = k == K_DS ? "IDirectSound" : k == K_BUFFER ? "IDirectSoundBuffer" : "IDirectSoundNotify";
    uint32_t n = k == K_DS ? sizeof ds_methods / sizeof *ds_methods
               : k == K_BUFFER ? sizeof buffer_methods / sizeof *buffer_methods
                               : sizeof notify_methods / sizeof *notify_methods;
    uint32_t t = rc_heap_alloc(4 * n, false);
    for (uint32_t i = 0; i < n; i++) {
        char buf[80];
        snprintf(buf, sizeof buf, "%s::%s", name, m[i]);
        rc_st32(t + 4 * i, rc_import_address(COM, rc_str(rc_proc_str(buf))));
    }
    tables[k] = t;
    return t;
}

static object *new_object(enum kind k)
{
    for (uint32_t i = 1; i < OBJECTS; i++) {
        if (objects[i].kind == K_FREE) {
            object *o = &objects[i];
            memset(o, 0, sizeof *o);
            o->kind = k;
            o->refs = 1;
            o->guest = rc_heap_alloc(8, false);
            rc_st32(o->guest, vtable(k));
            rc_st32(o->guest + 4, i);
            return o;
        }
    }
    rc_fatal("DirectSound: out of objects");
}

static object *self(rc_cpu *c)
{
    uint32_t g = RC_ARG(c, 0);
    uint32_t i = g ? rc_ld32(g + 4) : 0;
    if (!i || i >= OBJECTS || objects[i].kind == K_FREE) {
        rc_fatal("DirectSound: bad this pointer %#x", g);
    }
    return &objects[i];
}

static uint32_t rate_of(const object *b) { return b->freq ? b->freq : b->rate; }

static float gain_of(int32_t volume)
{
    return volume <= -10000 ? 0.0f : powf(10.0f, (float)volume / 2000.0f);
}

/* What of a buffer's stream has not played yet. */
static uint32_t queued_of(object *b)
{
    if (!b->pcm) {
        return 0;
    }
    if (!rc_clock_virtual()) {
        return rc_pcm_queued(b->pcm);
    }
    uint64_t now = rc_ticks_peek_ms();
    uint64_t bps = (uint64_t)rate_of(b) * b->align;
    uint64_t played = (now - b->vstart) * bps / 1000;
    if (played >= b->vput) {
        /* ran dry: what comes next starts playing now */
        b->vstart = now;
        b->vput = 0;
        return 0;
    }
    return b->vput - (uint32_t)played;
}

static void put_pcm(object *b, const void *data, uint32_t n)
{
    rc_pcm_put(b->pcm, data, n);
    b->vput += n;
}

static void clear_pcm(object *b)
{
    rc_pcm_clear(b->pcm);
    b->vstart = rc_ticks_peek_ms();
    b->vput = 0;
}

/* Keep about 80 ms of a playing buffer queued in its stream. */
static void feed(object *b)
{
    if (!b->playing || !b->pcm || !b->size) {
        return;
    }
    uint32_t want = rate_of(b) * b->align * 80 / 1000;
    want -= want % b->align;
    uint32_t queued = queued_of(b);
    while (queued < want) {
        uint32_t n = want - queued;
        if (n > b->size - b->feed) {
            n = b->size - b->feed;
        }
        put_pcm(b, rc_ptr(b->data + b->feed), n);
        queued += n;
        b->feed += n;
        if (b->feed >= b->size) {
            if (!b->looping) {
                b->playing = false; /* the rest drains from the stream */
                break;
            }
            b->feed = 0;
        }
    }
}

static void feed_all(rc_cpu *c)
{
    (void)c;
    for (uint32_t i = 1; i < OBJECTS; i++) {
        if (objects[i].kind == K_BUFFER) {
            feed(&objects[i]);
        }
    }
}

/* Where playback is: the feed position less what is still queued. */
static uint32_t play_cursor(object *b)
{
    if (!b->pcm || !b->size) {
        return 0;
    }
    uint32_t q = queued_of(b) % b->size;
    return (b->feed + b->size - q) % b->size;
}

static bool sounding(object *b) { return b->playing || queued_of(b) > 0; }

/* ---- IDirectSound ---- */

/* DirectSoundCreate(8)(guid, result, outer) */
static void k_DirectSoundCreate(rc_cpu *c)
{
    if (!audio_ok) {
        audio_ok = rc_audio_open(44100, 2);
    }
    if (!audio_ok) {
        if (RC_ARG(c, 1)) {
            rc_st32(RC_ARG(c, 1), 0);
        }
        RC_RETURN(c, DSERR_NODRIVER, 3);
    }
    if (!hooked) {
        rc_add_pump_hook(feed_all);
        hooked = true;
    }
    rc_st32(RC_ARG(c, 1), new_object(K_DS)->guest);
    RC_RETURN(c, DS_OK, 3);
}

/* DirectSoundEnumerateA(callback, context): no devices listed; the default one is used. */
static void k_DirectSoundEnumerateA(rc_cpu *c) { RC_RETURN(c, 0, 2); }

static void m_QueryInterface(rc_cpu *c)
{
    object *o = self(c);
    uint32_t iid = RC_ARG(c, 1);
    /* IDirectSoundNotify {b0210783-89cd-11d0-af08-00a0c925cd16} */
    if (o->kind == K_BUFFER && rc_ld32(iid) == 0xb0210783u) {
        rc_st32(RC_ARG(c, 2), new_object(K_NOTIFY)->guest);
        RC_RETURN(c, DS_OK, 3);
    }
    o->refs++;
    rc_st32(RC_ARG(c, 2), o->guest);
    RC_RETURN(c, DS_OK, 3);
}
static void m_AddRef(rc_cpu *c) { RC_RETURN(c, ++self(c)->refs, 1); }
static void m_Release(rc_cpu *c)
{
    object *o = self(c);
    uint32_t r = o->refs ? --o->refs : 0;
    if (!r && o->kind == K_BUFFER) {
        if (o->pcm) {
            rc_pcm_free(o->pcm);
        }
        o->kind = K_FREE;
    }
    RC_RETURN(c, r, 1);
}

static void set_format(object *b, uint32_t wfx)
{
    b->channels = rc_ld16(wfx + 2);
    b->rate = rc_ld32(wfx + 4);
    b->bits = rc_ld16(wfx + 14);
    b->align = rc_ld16(wfx + 12);
    if (!b->align) {
        b->align = b->channels * b->bits / 8;
    }
}

/* CreateSoundBuffer(this, DSBUFFERDESC*, result, outer) */
static void d_CreateSoundBuffer(rc_cpu *c)
{
    uint32_t desc = RC_ARG(c, 1);
    uint32_t flags = rc_ld32(desc + 4), bytes = rc_ld32(desc + 8), wfx = rc_ld32(desc + 16);
    object *b = new_object(K_BUFFER);
    b->primary = (flags & DSBCAPS_PRIMARYBUFFER) != 0;
    if (wfx) {
        set_format(b, wfx);
    } else {
        b->channels = 2, b->rate = 22050, b->bits = 16, b->align = 4;
    }
    if (!b->primary) {
        if (!bytes || !b->align || b->bits % 8 || !b->channels) {
            b->kind = K_FREE;
            RC_RETURN(c, DSERR_INVALIDPARAM, 4);
        }
        b->size = bytes - bytes % b->align;
        b->data = rc_heap_alloc(b->size, true);
    }
    rc_st32(RC_ARG(c, 2), b->guest);
    RC_RETURN(c, DS_OK, 4);
}

/* DuplicateSoundBuffer(this, original, result): the same memory, its own playback. */
static void d_DuplicateSoundBuffer(rc_cpu *c)
{
    uint32_t g = RC_ARG(c, 1);
    uint32_t i = g ? rc_ld32(g + 4) : 0;
    if (!i || i >= OBJECTS || objects[i].kind != K_BUFFER) {
        RC_RETURN(c, DSERR_INVALIDPARAM, 3);
    }
    object src = objects[i];
    object *b = new_object(K_BUFFER);
    b->data = src.data;
    b->size = src.size;
    b->rate = src.rate, b->channels = src.channels, b->bits = src.bits, b->align = src.align;
    b->freq = src.freq, b->volume = src.volume, b->pan = src.pan;
    rc_st32(RC_ARG(c, 2), b->guest);
    RC_RETURN(c, DS_OK, 3);
}

/* GetCaps(this, DSCAPS*): a 16-bit stereo device mixing in software. */
static void d_GetCaps(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 1);
    uint32_t size = rc_ld32(p);
    memset(rc_ptr(p + 4), 0, size > 4 ? size - 4 : 0);
    rc_st32(p + 4, 0x0f00 | 0x0400); /* DSCAPS_PRIMARY*, SECONDARY* (mono/stereo, 8/16 bit) */
    rc_st32(p + 8, 100);             /* dwMinSecondarySampleRate */
    rc_st32(p + 12, 100000);         /* dwMaxSecondarySampleRate */
    rc_st32(p + 16, 1);              /* dwPrimaryBuffers */
    RC_RETURN(c, DS_OK, 2);
}

static void d_GetSpeakerConfig(rc_cpu *c)
{
    rc_st32(RC_ARG(c, 1), 4); /* DSSPEAKER_STEREO */
    RC_RETURN(c, DS_OK, 2);
}

static void ok1(rc_cpu *c) { RC_RETURN(c, DS_OK, 1); }
static void ok2(rc_cpu *c) { RC_RETURN(c, DS_OK, 2); }
static void ok3(rc_cpu *c) { RC_RETURN(c, DS_OK, 3); }

/* ---- IDirectSoundBuffer ---- */

/* GetCaps(this, DSBCAPS*) */
static void b_GetCaps(rc_cpu *c)
{
    object *b = self(c);
    uint32_t p = RC_ARG(c, 1);
    rc_st32(p + 4, b->primary ? DSBCAPS_PRIMARYBUFFER : 0);
    rc_st32(p + 8, b->size);
    rc_st32(p + 12, 0);
    rc_st32(p + 16, 0);
    RC_RETURN(c, DS_OK, 2);
}

/* GetCurrentPosition(this, play*, write*) */
static void b_GetCurrentPosition(rc_cpu *c)
{
    object *b = self(c);
    feed(b);
    uint32_t play = play_cursor(b);
    if (RC_ARG(c, 1)) {
        rc_st32(RC_ARG(c, 1), play);
    }
    if (RC_ARG(c, 2)) {
        rc_st32(RC_ARG(c, 2), b->size ? b->feed % b->size : 0);
    }
    RC_RETURN(c, DS_OK, 3);
}

/* GetFormat(this, WAVEFORMATEX*, size, written*) */
static void b_GetFormat(rc_cpu *c)
{
    object *b = self(c);
    uint32_t p = RC_ARG(c, 1), cap = RC_ARG(c, 2);
    if (p && cap >= 16) {
        rc_st16(p, 1);
        rc_st16(p + 2, (uint16_t)b->channels);
        rc_st32(p + 4, b->rate);
        rc_st32(p + 8, b->rate * b->align);
        rc_st16(p + 12, (uint16_t)b->align);
        rc_st16(p + 14, (uint16_t)b->bits);
        if (cap >= 18) {
            rc_st16(p + 16, 0);
        }
    }
    if (RC_ARG(c, 3)) {
        rc_st32(RC_ARG(c, 3), 18);
    }
    RC_RETURN(c, DS_OK, 4);
}

static void b_GetVolume(rc_cpu *c)
{
    rc_st32(RC_ARG(c, 1), (uint32_t)self(c)->volume);
    RC_RETURN(c, DS_OK, 2);
}
static void b_GetPan(rc_cpu *c)
{
    rc_st32(RC_ARG(c, 1), (uint32_t)self(c)->pan);
    RC_RETURN(c, DS_OK, 2);
}
static void b_GetFrequency(rc_cpu *c)
{
    rc_st32(RC_ARG(c, 1), rate_of(self(c)));
    RC_RETURN(c, DS_OK, 2);
}

static void b_GetStatus(rc_cpu *c)
{
    object *b = self(c);
    feed(b);
    uint32_t st = sounding(b) ? DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0) : 0;
    rc_st32(RC_ARG(c, 1), st);
    RC_RETURN(c, DS_OK, 2);
}

/* Lock(this, offset, bytes, p1*, n1*, p2*, n2*, flags) */
static void b_Lock(rc_cpu *c)
{
    object *b = self(c);
    uint32_t off = RC_ARG(c, 1), n = RC_ARG(c, 2), flags = RC_ARG(c, 7);
    if (!b->size) {
        RC_RETURN(c, DSERR_INVALIDPARAM, 8);
    }
    if (flags & DSBLOCK_FROMWRITECURSOR) {
        off = b->feed % b->size;
    }
    if (flags & DSBLOCK_ENTIREBUFFER || n > b->size) {
        n = b->size;
    }
    off %= b->size;
    uint32_t first = n < b->size - off ? n : b->size - off;
    rc_st32(RC_ARG(c, 3), b->data + off);
    rc_st32(RC_ARG(c, 4), first);
    if (RC_ARG(c, 5)) {
        rc_st32(RC_ARG(c, 5), first < n ? b->data : 0);
    }
    if (RC_ARG(c, 6)) {
        rc_st32(RC_ARG(c, 6), n - first);
    }
    RC_RETURN(c, DS_OK, 8);
}

static void b_Unlock(rc_cpu *c) { RC_RETURN(c, DS_OK, 5); }

/* Play(this, reserved, priority, flags) */
static void b_Play(rc_cpu *c)
{
    object *b = self(c);
    if (b->primary) {
        RC_RETURN(c, DS_OK, 4);
    }
    b->looping = (RC_ARG(c, 3) & DSBPLAY_LOOPING) != 0;
    if (!b->pcm || b->pcm_rate != rate_of(b)) {
        if (b->pcm) {
            rc_pcm_free(b->pcm);
        }
        b->pcm = rc_pcm_open((int)rate_of(b), (int)b->channels, (int)b->bits);
        b->pcm_rate = rate_of(b);
        if (b->pcm) {
            rc_pcm_gain(b->pcm, gain_of(b->volume));
        }
    }
    if (b->pcm) {
        rc_pcm_pause(b->pcm, false);
    }
    b->playing = true;
    feed(b);
    RC_RETURN(c, DS_OK, 4);
}

static void b_Stop(rc_cpu *c)
{
    object *b = self(c);
    if (b->pcm) {
        /* What is queued has not played: the position goes back to where playback is. */
        b->feed = play_cursor(b);
        clear_pcm(b);
    }
    b->playing = false;
    RC_RETURN(c, DS_OK, 1);
}

static void b_SetCurrentPosition(rc_cpu *c)
{
    object *b = self(c);
    if (b->size) {
        b->feed = (RC_ARG(c, 1) % b->size) / b->align * b->align;
    }
    if (b->pcm) {
        clear_pcm(b);
    }
    feed(b);
    RC_RETURN(c, DS_OK, 2);
}

static void b_SetFormat(rc_cpu *c)
{
    object *b = self(c);
    if (b->primary && RC_ARG(c, 1)) {
        set_format(b, RC_ARG(c, 1));
    }
    RC_RETURN(c, DS_OK, 2);
}

static void b_SetVolume(rc_cpu *c)
{
    object *b = self(c);
    b->volume = (int32_t)RC_ARG(c, 1);
    if (b->pcm) {
        rc_pcm_gain(b->pcm, gain_of(b->volume));
    }
    RC_RETURN(c, DS_OK, 2);
}

static void b_SetPan(rc_cpu *c)
{
    self(c)->pan = (int32_t)RC_ARG(c, 1);
    RC_RETURN(c, DS_OK, 2);
}

/* SetFrequency(this, hz): 0 is the format's; takes effect at the next Play. */
static void b_SetFrequency(rc_cpu *c)
{
    object *b = self(c);
    b->freq = RC_ARG(c, 1);
    if (b->playing && b->pcm && b->pcm_rate != rate_of(b)) {
        uint32_t at = play_cursor(b);
        rc_pcm_free(b->pcm);
        b->pcm = rc_pcm_open((int)rate_of(b), (int)b->channels, (int)b->bits);
        b->pcm_rate = rate_of(b);
        b->feed = at;
        if (b->pcm) {
            rc_pcm_gain(b->pcm, gain_of(b->volume));
        }
        feed(b);
    }
    RC_RETURN(c, DS_OK, 2);
}

/* SetNotificationPositions(this, count, positions): no events are signalled. */
static void n_SetNotificationPositions(rc_cpu *c) { RC_RETURN(c, DS_OK, 3); }

#define EACH(m, fn)                                                                                \
    {COM, "IDirectSound::" m, fn}, {COM, "IDirectSoundBuffer::" m, fn}, {COM, "IDirectSoundNotify::" m, fn}

const rc_import_def rc_dsound_defs[] = {
    {DS, "#1", k_DirectSoundCreate},
    {DS, "DirectSoundCreate", k_DirectSoundCreate},
    {DS, "#11", k_DirectSoundCreate},
    {DS, "DirectSoundCreate8", k_DirectSoundCreate},
    {DS, "#2", k_DirectSoundEnumerateA},
    {DS, "DirectSoundEnumerateA", k_DirectSoundEnumerateA},
    EACH("QueryInterface", m_QueryInterface),
    EACH("AddRef", m_AddRef),
    EACH("Release", m_Release),
    {COM, "IDirectSound::CreateSoundBuffer", d_CreateSoundBuffer},
    {COM, "IDirectSound::DuplicateSoundBuffer", d_DuplicateSoundBuffer},
    {COM, "IDirectSound::GetCaps", d_GetCaps},
    {COM, "IDirectSound::SetCooperativeLevel", ok3},
    {COM, "IDirectSound::Compact", ok1},
    {COM, "IDirectSound::GetSpeakerConfig", d_GetSpeakerConfig},
    {COM, "IDirectSound::SetSpeakerConfig", ok2},
    {COM, "IDirectSound::Initialize", ok2},
    {COM, "IDirectSoundBuffer::GetCaps", b_GetCaps},
    {COM, "IDirectSoundBuffer::GetCurrentPosition", b_GetCurrentPosition},
    {COM, "IDirectSoundBuffer::GetFormat", b_GetFormat},
    {COM, "IDirectSoundBuffer::GetVolume", b_GetVolume},
    {COM, "IDirectSoundBuffer::GetPan", b_GetPan},
    {COM, "IDirectSoundBuffer::GetFrequency", b_GetFrequency},
    {COM, "IDirectSoundBuffer::GetStatus", b_GetStatus},
    {COM, "IDirectSoundBuffer::Lock", b_Lock},
    {COM, "IDirectSoundBuffer::Unlock", b_Unlock},
    {COM, "IDirectSoundBuffer::Play", b_Play},
    {COM, "IDirectSoundBuffer::Stop", b_Stop},
    {COM, "IDirectSoundBuffer::SetCurrentPosition", b_SetCurrentPosition},
    {COM, "IDirectSoundBuffer::SetFormat", b_SetFormat},
    {COM, "IDirectSoundBuffer::SetVolume", b_SetVolume},
    {COM, "IDirectSoundBuffer::SetPan", b_SetPan},
    {COM, "IDirectSoundBuffer::SetFrequency", b_SetFrequency},
    {COM, "IDirectSoundBuffer::Restore", ok1},
    {COM, "IDirectSoundNotify::SetNotificationPositions", n_SetNotificationPositions},
};
const uint32_t rc_dsound_count = sizeof rc_dsound_defs / sizeof rc_dsound_defs[0];
