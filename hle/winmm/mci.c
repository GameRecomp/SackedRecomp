/* SPDX-License-Identifier: MIT */
/*
 * WinMM's CD audio (MCI "cdaudio") and mixer, for games that play their music from the CD.
 *
 * Track n plays MUSIC/Track<nn>.ogg (or .flac, .wav) from the install: the files GOG ships
 * in place of the CD's audio tracks. Track 1 is the data track. Without the files the drive
 * has no audio tracks and the game plays without music. Time formats: milliseconds and TMSF
 * (track in the low byte); positions are track starts. A play with MCI_NOTIFY posts
 * MM_MCINOTIFY to the window when the track ends, as the CD driver does.
 *
 * The mixer has no devices, so code that would set the CD or wave volume through it does
 * without (the volume is set on the music itself).
 */
#include "winmm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel32/k32.h"
#include "rc/audio.h"
#include "rc/hostfs.h"
#include "rc/log.h"
#include "rc/process.h"
#include "rc/vm.h"
#include "user32/user32.h"

#define WMM "WINMM.dll"
#define MMSYSERR_NODRIVER 6u

#define MCI_OPEN 0x0803u
#define MCI_CLOSE 0x0804u
#define MCI_PLAY 0x0806u
#define MCI_SEEK 0x0807u
#define MCI_STOP 0x0808u
#define MCI_PAUSE 0x0809u
#define MCI_SET 0x080du
#define MCI_STATUS 0x0814u
#define MCI_RESUME 0x0855u

#define MCI_NOTIFY 0x1u
#define MCI_FROM 0x4u
#define MCI_TO 0x8u
#define MCI_TRACK 0x10u
#define MCI_SET_TIME_FORMAT 0x400u
#define MCI_STATUS_ITEM 0x100u
#define MCI_OPEN_TYPE 0x2000u
#define MCI_OPEN_TYPE_ID 0x1000u

#define MCI_STATUS_LENGTH 1u
#define MCI_STATUS_POSITION 2u
#define MCI_STATUS_NUMBER_OF_TRACKS 3u
#define MCI_STATUS_MODE 4u
#define MCI_STATUS_MEDIA_PRESENT 5u
#define MCI_STATUS_TIME_FORMAT 6u
#define MCI_STATUS_READY 7u
#define MCI_STATUS_CURRENT_TRACK 8u
#define MCI_CDA_STATUS_TYPE_TRACK 0x4001u
#define MCI_CDA_TRACK_AUDIO 1066u
#define MCI_CDA_TRACK_OTHER 1067u

#define MCI_MODE_STOP 525u
#define MCI_MODE_PLAY 526u
#define MCI_MODE_PAUSE 529u
#define MCI_FORMAT_MILLISECONDS 0u
#define MCI_FORMAT_TMSF 10u

#define MCIERR_INVALID_DEVICE_ID 257u
#define MCIERR_UNRECOGNIZED_COMMAND 261u
#define MCIERR_OUTOFRANGE 264u
#define MCIERR_DEVICE_NOT_READY 276u
#define MM_MCINOTIFY 0x3b9u
#define MCI_NOTIFY_SUCCESSFUL 1u

#define DEVICE_ID 1u
#define TRACKS 99

static struct {
    bool open;
    uint32_t format;
    int tracks;           /* highest track number with a file (0: none) */
    bool present[TRACKS + 1];
    uint32_t length[TRACKS + 1];  /* ms, 0 until known */
    int current;          /* track playing or paused, 0 for none */
    int to;               /* last track of the current play */
    bool paused;
    uint32_t notify_hwnd; /* 0: no notification pending */
    rc_sound *sound;
    rc_track *track;
    float gain;
} cd = {.gain = 1.0f};

/* The host path of track n's file; false if there is none. */
static bool track_path(int n, char *path)
{
    static const char *const ext[] = {"ogg", "flac", "wav"};
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        char rel[64];
        snprintf(rel, sizeof rel, "MUSIC/Track%02d.%s", n, ext[i]);
        if (rc_host_resolve(rc_cfg->game_dir, rel, false, path) && rc_host_exists(path, NULL, NULL)) {
            return true;
        }
    }
    return false;
}

static rc_sound *load_track(int n)
{
    char path[RC_PATH_MAX];
    if (track_path(n, path)) {
        uint32_t size = 0;
        uint8_t *data = rc_host_read(path, &size);
        rc_sound *s = data ? rc_sound_load(data, size) : NULL;
        free(data);
        if (s) {
            return s;
        }
    }
    return NULL;
}

/* A track's length in milliseconds (decoded once, when first asked for). */
static uint32_t length(int n)
{
    if (n < 1 || n > cd.tracks || !cd.present[n]) {
        return 0;
    }
    if (!cd.length[n]) {
        rc_sound *s = load_track(n);
        cd.length[n] = s && rc_sound_ms(s) ? rc_sound_ms(s) : 1;
        rc_sound_free(s);
    }
    return cd.length[n];
}

/* Which tracks exist. Decoding needs the audio device, which the game may open later
 * (through Miles): open it now; the platform keeps the first opening. */
static void scan(void)
{
    char path[RC_PATH_MAX];
    cd.tracks = 0;
    for (int n = 2; n <= TRACKS; n++) {
        cd.present[n] = track_path(n, path);
        cd.length[n] = 0;
        if (cd.present[n]) {
            cd.tracks = n;
        }
    }
    if (cd.tracks) {
        cd.present[1] = true;
        cd.length[1] = 1; /* the data track */
        rc_audio_open(44100, 2);
    }
}

static void stop(void)
{
    if (cd.track) {
        rc_track_stop(cd.track);
    }
    if (cd.sound) {
        rc_sound_free(cd.sound);
        cd.sound = NULL;
    }
    cd.current = 0;
    cd.paused = false;
}

static bool start(int n)
{
    stop();
    if (n < 2 || n > cd.tracks || !cd.present[n]) {
        return false;
    }
    if (!cd.track) {
        cd.track = rc_track_new();
    }
    cd.sound = load_track(n);
    if (!cd.track || !cd.sound || !rc_track_play(cd.track, cd.sound, 1)) {
        stop();
        return false;
    }
    rc_track_gain(cd.track, cd.gain);
    cd.current = n;
    return true;
}

/* At every message pump: the next track of a play, or the notification at its end. */
static void poll(rc_cpu *c)
{
    (void)c;
    if (!cd.current || cd.paused || (cd.track && rc_track_playing(cd.track))) {
        return;
    }
    if (cd.current < cd.to && start(cd.current + 1)) {
        return;
    }
    stop();
    if (cd.notify_hwnd) {
        uint32_t hwnd = cd.notify_hwnd;
        cd.notify_hwnd = 0;
        rc_post_message(hwnd, MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL, DEVICE_ID);
    }
}

static bool is_cdaudio(const char *s)
{
    const char *want = "cdaudio";
    if (!s) {
        return false;
    }
    for (; *want; s++, want++) {
        char ch = *s >= 'A' && *s <= 'Z' ? (char)(*s - 'A' + 'a') : *s;
        if (ch != *want) {
            return false;
        }
    }
    return *s == 0;
}

static int track_of(uint32_t pos)
{
    return cd.format == MCI_FORMAT_TMSF ? (int)(pos & 0xff) : 0;
}

static uint32_t status(uint32_t flags, uint32_t item, uint32_t trk, uint32_t *ret)
{
    switch (item) {
    case MCI_STATUS_NUMBER_OF_TRACKS:
        *ret = (uint32_t)cd.tracks;
        return 0;
    case MCI_STATUS_MEDIA_PRESENT:
    case MCI_STATUS_READY:
        *ret = cd.tracks ? 1 : 0;
        return 0;
    case MCI_STATUS_MODE:
        *ret = !cd.current ? MCI_MODE_STOP : cd.paused ? MCI_MODE_PAUSE : MCI_MODE_PLAY;
        return 0;
    case MCI_STATUS_TIME_FORMAT:
        *ret = cd.format;
        return 0;
    case MCI_STATUS_CURRENT_TRACK:
        *ret = (uint32_t)(cd.current ? cd.current : 1);
        return 0;
    case MCI_STATUS_LENGTH:
    case MCI_STATUS_POSITION:
    case MCI_CDA_STATUS_TYPE_TRACK: {
        int n = (flags & MCI_TRACK) ? (int)trk : cd.current;
        if ((flags & MCI_TRACK) && (n < 1 || n > cd.tracks)) {
            return MCIERR_OUTOFRANGE;
        }
        if (item == MCI_CDA_STATUS_TYPE_TRACK) {
            *ret = n >= 2 && cd.present[n] ? MCI_CDA_TRACK_AUDIO : MCI_CDA_TRACK_OTHER;
        } else if (item == MCI_STATUS_LENGTH) {
            uint32_t ms = 0;
            if (flags & MCI_TRACK) {
                ms = length(n);
            } else {
                for (int k = 1; k <= cd.tracks; k++) {
                    ms += length(k);
                }
            }
            /* TMSF lengths are MSF: minutes, seconds, frames (75 a second). */
            *ret = cd.format == MCI_FORMAT_TMSF
                       ? (ms / 60000) | ((ms / 1000 % 60) << 8) | ((ms % 1000 * 75 / 1000) << 16)
                       : ms;
        } else {
            *ret = cd.format == MCI_FORMAT_TMSF ? (uint32_t)(n ? n : 1) : 0;
        }
        return 0;
    }
    default:
        return MCIERR_UNRECOGNIZED_COMMAND;
    }
}

/* mciSendCommandA(device, message, flags, params) */
static void k_mciSendCommandA(rc_cpu *c)
{
    uint32_t dev = RC_ARG(c, 0), msg = RC_ARG(c, 1), flags = RC_ARG(c, 2), p = RC_ARG(c, 3);
    uint32_t r = 0;
    if (msg != MCI_OPEN && (dev != DEVICE_ID || !cd.open)) {
        RC_RETURN(c, MCIERR_INVALID_DEVICE_ID, 4);
    }
    switch (msg) {
    case MCI_OPEN:
        /* Only the CD: by name ("cdaudio") or by the type id MCI_DEVTYPE_CD_AUDIO (516). */
        if (flags & MCI_OPEN_TYPE) {
            uint32_t type = rc_ld32(p + 8);
            bool cdaudio = (flags & MCI_OPEN_TYPE_ID) ? (type & 0xffff) == 516 : is_cdaudio(rc_str(type));
            if (!cdaudio) {
                RC_RETURN(c, MMSYSERR_NODRIVER, 4);
            }
        }
        if (!cd.open) {
            scan();
            rc_add_pump_hook(poll);
        }
        cd.open = true;
        rc_st32(p + 4, DEVICE_ID);
        break;
    case MCI_CLOSE:
        stop();
        cd.open = false;
        break;
    case MCI_SET:
        if (flags & MCI_SET_TIME_FORMAT) {
            cd.format = rc_ld32(p + 4);
        }
        break;
    case MCI_STATUS: {
        uint32_t ret = 0;
        if (!(flags & MCI_STATUS_ITEM)) {
            r = MCIERR_UNRECOGNIZED_COMMAND;
            break;
        }
        r = status(flags, rc_ld32(p + 8), rc_ld32(p + 12), &ret);
        if (!r) {
            rc_st32(p + 4, ret);
        }
        break;
    }
    case MCI_PLAY: {
        int from = (flags & MCI_FROM) ? track_of(rc_ld32(p + 4)) : (cd.current ? cd.current : 2);
        int to = (flags & MCI_TO) ? track_of(rc_ld32(p + 8)) : cd.tracks;
        if (to < from) {
            to = from;
        }
        /* A play to the start of a track ends with the one before it (TMSF with 0:00:00). */
        if ((flags & MCI_TO) && (rc_ld32(p + 8) & 0xffffff00u) == 0 && to > from) {
            to--;
        }
        if (!cd.tracks) {
            r = MCIERR_DEVICE_NOT_READY;
            break;
        }
        bool ok = start(from);
        if (rc_trace("sound")) {
            rc_log("[sound] cd play %#x from %#x to %#x (tracks %d to %d of %d, format %u): %s", flags,
                   rc_ld32(p + 4), rc_ld32(p + 8), from, to, cd.tracks, cd.format, ok ? "playing" : "failed");
        }
        if (!ok) {
            r = MCIERR_OUTOFRANGE;
            break;
        }
        cd.to = to;
        cd.notify_hwnd = (flags & MCI_NOTIFY) ? rc_ld32(p) : 0;
        break;
    }
    case MCI_STOP:
    case MCI_SEEK:
        stop();
        cd.notify_hwnd = 0;
        break;
    case MCI_PAUSE:
        if (cd.current && cd.track) {
            rc_track_pause(cd.track, true);
            cd.paused = true;
        }
        break;
    case MCI_RESUME:
        if (cd.current && cd.track) {
            rc_track_pause(cd.track, false);
            cd.paused = false;
        }
        break;
    default:
        r = MCIERR_UNRECOGNIZED_COMMAND;
        break;
    }
    RC_RETURN(c, r, 4);
}

/* mciGetErrorStringA(error, buffer, length) */
static void k_mciGetErrorStringA(rc_cpu *c)
{
    char text[48];
    snprintf(text, sizeof text, "MCI error %u", RC_ARG(c, 0));
    rc_put_str(RC_ARG(c, 1), RC_ARG(c, 2), text);
    RC_RETURN(c, 1, 3);
}

static void k_mixerGetNumDevs(rc_cpu *c) { RC_RETURN(c, 0, 0); }
static void k_nodriver1(rc_cpu *c) { RC_RETURN(c, MMSYSERR_NODRIVER, 1); }
static void k_nodriver3(rc_cpu *c) { RC_RETURN(c, MMSYSERR_NODRIVER, 3); }
static void k_nodriver5(rc_cpu *c) { RC_RETURN(c, MMSYSERR_NODRIVER, 5); }

/* ---- mmio: RIFF files for reading (the WAV files of DirectSound samples) ----
 *
 * A file opened for reading is read whole into host memory. The program reads through mmioRead,
 * or straight from the I/O buffer (mmioGetInfo, mmioAdvance, mmioSetInfo, as the DirectX SDK's
 * wave reader does); the buffer is in guest memory. Chunks are found with mmioDescend and left
 * with mmioAscend. Writing is not supported. */
#define MMIOERR_CANNOTOPEN 257u
#define MMIOERR_CANNOTREAD 263u
#define MMIOERR_CHUNKNOTFOUND 265u
#define MMIO_ALLOCBUF 0x00010000u
#define MMIO_RWMODE 0x00000003u
#define MMIO_FINDCHUNK 0x0010u
#define MMIO_FINDRIFF 0x0020u
#define MMIO_FINDLIST 0x0040u
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

#define MMIO_FILES 16
static struct mmio_file {
    uint8_t *data;
    uint32_t size, pos;
    uint32_t buf, cap;      /* the guest I/O buffer and its size (0: unbuffered) */
    uint32_t buf_off, buf_len;
    bool open;
} mmio_files[MMIO_FILES];

static struct mmio_file *mmio_get(uint32_t h)
{
    return h && h <= MMIO_FILES && mmio_files[h - 1].open ? &mmio_files[h - 1] : NULL;
}

/* The buffer no longer matches the position (after a read, seek or chunk move). */
static void mmio_reset(struct mmio_file *f)
{
    f->buf_off = f->pos;
    f->buf_len = 0;
}

/* mmioOpenA(name, info, flags) */
static void k_mmioOpenA(rc_cpu *c)
{
    uint32_t name = RC_ARG(c, 0), info = RC_ARG(c, 1), flags = RC_ARG(c, 2);
    char rel[512], host[1024];
    uint32_t err = 0, h = 0;
    if (!name || (flags & MMIO_RWMODE) != 0) {
        err = MMIOERR_CANNOTOPEN; /* by I/O procedure or for writing: not supported */
    } else if (!rc_k32_guest_rel(rc_str(name), rel, sizeof rel) ||
               !rc_k32_find_existing(rel, host, NULL, NULL)) {
        err = MMIOERR_CANNOTOPEN;
    } else {
        for (uint32_t i = 0; i < MMIO_FILES && !h; i++) {
            if (!mmio_files[i].open) {
                h = i + 1;
            }
        }
        uint32_t size = 0;
        uint8_t *data = h ? rc_host_read(host, &size) : NULL;
        if (!data) {
            err = MMIOERR_CANNOTOPEN;
            h = 0;
        } else {
            struct mmio_file *f = &mmio_files[h - 1];
            memset(f, 0, sizeof *f);
            f->open = true;
            f->data = data;
            f->size = size;
            if (flags & MMIO_ALLOCBUF) {
                f->cap = info && rc_ld32(info + 20) ? rc_ld32(info + 20) : 8192;
                f->buf = rc_heap_alloc(f->cap, true);
            } else if (info && rc_ld32(info + 24) && rc_ld32(info + 20)) {
                f->cap = rc_ld32(info + 20);
                f->buf = rc_ld32(info + 24);
            }
        }
    }
    if (info) {
        rc_st32(info + 12, err);
    }
    RC_RETURN(c, h, 3);
}

static void k_mmioClose(rc_cpu *c)
{
    struct mmio_file *f = mmio_get(RC_ARG(c, 0));
    if (!f) {
        RC_RETURN(c, MMIOERR_CANNOTREAD, 2);
    }
    free(f->data);
    f->open = false;
    RC_RETURN(c, 0, 2);
}

/* mmioRead(h, buffer, n): the bytes read, 0 at the end, -1 on error. */
static void k_mmioRead(rc_cpu *c)
{
    struct mmio_file *f = mmio_get(RC_ARG(c, 0));
    int32_t n = (int32_t)RC_ARG(c, 2);
    if (!f || n < 0) {
        RC_RETURN(c, 0xffffffffu, 3);
    }
    uint32_t left = f->pos < f->size ? f->size - f->pos : 0;
    uint32_t k = (uint32_t)n < left ? (uint32_t)n : left;
    memcpy(rc_ptr(RC_ARG(c, 1)), f->data + f->pos, k);
    f->pos += k;
    mmio_reset(f);
    RC_RETURN(c, k, 3);
}

/* mmioSeek(h, offset, origin): the new position. */
static void k_mmioSeek(rc_cpu *c)
{
    struct mmio_file *f = mmio_get(RC_ARG(c, 0));
    int32_t off = (int32_t)RC_ARG(c, 1);
    uint32_t origin = RC_ARG(c, 2);
    if (!f || origin > 2) {
        RC_RETURN(c, 0xffffffffu, 3);
    }
    int64_t base = origin == 0 ? 0 : origin == 1 ? (int64_t)f->pos : (int64_t)f->size;
    int64_t to = base + off;
    if (to < 0) {
        RC_RETURN(c, 0xffffffffu, 3);
    }
    f->pos = (uint32_t)to;
    mmio_reset(f);
    RC_RETURN(c, f->pos, 3);
}

/* The MMIOINFO the program sees for `f`. */
static void mmio_fill_info(struct mmio_file *f, uint32_t h, uint32_t info)
{
    memset(rc_ptr(info), 0, 72);
    rc_st32(info + 20, f->cap);
    rc_st32(info + 24, f->buf);
    rc_st32(info + 28, f->buf + (f->pos - f->buf_off));
    rc_st32(info + 32, f->buf + f->buf_len);
    rc_st32(info + 36, f->buf + f->buf_len);
    rc_st32(info + 40, f->buf_off);
    rc_st32(info + 44, f->buf_off + f->buf_len);
    rc_st32(info + 68, h);
}

static void k_mmioGetInfo(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0);
    struct mmio_file *f = mmio_get(h);
    if (!f) {
        RC_RETURN(c, MMIOERR_CANNOTREAD, 3);
    }
    mmio_fill_info(f, h, RC_ARG(c, 1));
    RC_RETURN(c, 0, 3);
}

/* mmioSetInfo(h, info, flags): the program read up to info->pchNext. */
static void k_mmioSetInfo(rc_cpu *c)
{
    struct mmio_file *f = mmio_get(RC_ARG(c, 0));
    uint32_t info = RC_ARG(c, 1);
    if (!f) {
        RC_RETURN(c, MMIOERR_CANNOTREAD, 3);
    }
    if (f->buf) {
        f->pos = f->buf_off + (rc_ld32(info + 28) - f->buf);
    }
    RC_RETURN(c, 0, 3);
}

/* mmioAdvance(h, info, flags): the next buffer, from where the program read to. */
static void k_mmioAdvance(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 0), info = RC_ARG(c, 1);
    struct mmio_file *f = mmio_get(h);
    if (!f || !f->buf) {
        RC_RETURN(c, MMIOERR_CANNOTREAD, 3);
    }
    if (info) {
        f->pos = f->buf_off + (rc_ld32(info + 28) - f->buf);
    }
    f->buf_off = f->pos;
    uint32_t left = f->pos < f->size ? f->size - f->pos : 0;
    f->buf_len = left < f->cap ? left : f->cap;
    memcpy(rc_ptr(f->buf), f->data + f->pos, f->buf_len);
    if (info) {
        mmio_fill_info(f, h, info);
    }
    RC_RETURN(c, 0, 3);
}

/* mmioDescend(h, ck, parent, flags): into the next chunk, or the next one with the asked id
 * (and form type for RIFF and LIST), within the parent. */
static void k_mmioDescend(rc_cpu *c)
{
    struct mmio_file *f = mmio_get(RC_ARG(c, 0));
    uint32_t ck = RC_ARG(c, 1), parent = RC_ARG(c, 2), flags = RC_ARG(c, 3);
    if (!f) {
        RC_RETURN(c, MMIOERR_CANNOTREAD, 4);
    }
    uint32_t end = f->size;
    if (parent) {
        uint32_t pend = rc_ld32(parent + 12) + rc_ld32(parent + 4);
        end = pend < end ? pend : end;
    }
    uint32_t want_id = rc_ld32(ck), want_type = rc_ld32(ck + 8);
    if (flags & MMIO_FINDRIFF) {
        want_id = FOURCC('R', 'I', 'F', 'F');
    } else if (flags & MMIO_FINDLIST) {
        want_id = FOURCC('L', 'I', 'S', 'T');
    }
    bool find = (flags & (MMIO_FINDCHUNK | MMIO_FINDRIFF | MMIO_FINDLIST)) != 0;
    uint32_t at = f->pos;
    while (at + 8 <= end) {
        uint32_t id, size, type = 0;
        memcpy(&id, f->data + at, 4);
        memcpy(&size, f->data + at + 4, 4);
        bool form = id == FOURCC('R', 'I', 'F', 'F') || id == FOURCC('L', 'I', 'S', 'T');
        if (form && at + 12 <= end) {
            memcpy(&type, f->data + at + 8, 4);
        }
        bool match = !find || (id == want_id &&
                               (!(flags & (MMIO_FINDRIFF | MMIO_FINDLIST)) || type == want_type));
        if (match) {
            rc_st32(ck, id);
            rc_st32(ck + 4, size);
            rc_st32(ck + 8, type);
            rc_st32(ck + 12, at + 8);
            rc_st32(ck + 16, 0);
            f->pos = at + 8 + (form ? 4 : 0);
            mmio_reset(f);
            RC_RETURN(c, 0, 4);
        }
        at += 8 + size + (size & 1);
    }
    RC_RETURN(c, MMIOERR_CHUNKNOTFOUND, 4);
}

/* mmioAscend(h, ck, flags): past the end of the chunk (padded to an even size). */
static void k_mmioAscend(rc_cpu *c)
{
    struct mmio_file *f = mmio_get(RC_ARG(c, 0));
    uint32_t ck = RC_ARG(c, 1);
    if (!f) {
        RC_RETURN(c, MMIOERR_CANNOTREAD, 3);
    }
    uint32_t size = rc_ld32(ck + 4);
    f->pos = rc_ld32(ck + 12) + size + (size & 1);
    mmio_reset(f);
    RC_RETURN(c, 0, 3);
}

const rc_import_def rc_winmm_mci_defs[] = {
    {WMM, "mmioOpenA", k_mmioOpenA},
    {WMM, "mmioClose", k_mmioClose},
    {WMM, "mmioRead", k_mmioRead},
    {WMM, "mmioSeek", k_mmioSeek},
    {WMM, "mmioGetInfo", k_mmioGetInfo},
    {WMM, "mmioSetInfo", k_mmioSetInfo},
    {WMM, "mmioAdvance", k_mmioAdvance},
    {WMM, "mmioDescend", k_mmioDescend},
    {WMM, "mmioAscend", k_mmioAscend},
    {WMM, "mciSendCommandA", k_mciSendCommandA},
    {WMM, "mciGetErrorStringA", k_mciGetErrorStringA},
    {WMM, "mixerGetNumDevs", k_mixerGetNumDevs},
    {WMM, "mixerOpen", k_nodriver5},
    {WMM, "mixerClose", k_nodriver1},
    {WMM, "mixerGetLineInfoA", k_nodriver3},
    {WMM, "mixerGetLineControlsA", k_nodriver3},
    {WMM, "mixerGetControlDetailsA", k_nodriver3},
    {WMM, "mixerSetControlDetails", k_nodriver3},
};
const uint32_t rc_winmm_mci_count = sizeof rc_winmm_mci_defs / sizeof rc_winmm_mci_defs[0];
