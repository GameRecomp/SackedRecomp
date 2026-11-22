/* SPDX-License-Identifier: MIT */
/*
 * DirectShow, for playing a movie file: the filter graph and the interfaces games use with it
 * (IGraphBuilder, IMediaControl, IMediaEvent(Ex), IVideoWindow, IBasicAudio, IBasicVideo,
 * IMediaPosition, IMediaSeeking).
 *
 * RenderFile opens an MPEG-1 system stream with pl_mpeg (third_party/pl_mpeg). While the graph
 * runs, the movie is decoded at the clock's pace whenever the program pumps messages or asks
 * for events: its pictures go to the window as they are (the game's own frames are not drawn
 * meanwhile) and its sound into a platform stream. At the end the graph reports EC_COMPLETE,
 * through GetEvent and, if the program asked for it, a message to its window.
 *
 * COM objects are as in ddraw.c; every interface of the graph is its own guest block that
 * names the graph.
 */
#include "dshow.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wtype-limits"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "pl_mpeg.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "ddraw/ddraw.h"
#include "kernel32/k32.h"
#include "rc/audio.h"
#include "rc/hostfs.h"
#include "rc/log.h"
#include "rc/platform.h"
#include "rc/process.h"
#include "rc/vm.h"
#include "user32/user32.h"

#define COM "DSHOW_COM"
#define S_OK 0u
#define S_FALSE 1u
#define E_FAIL 0x80004005u
#define E_ABORT 0x80004004u
#define E_NOINTERFACE 0x80004002u
#define VFW_E_NOT_FOUND 0x80040216u
#define EC_COMPLETE 1u

enum iface { I_GRAPH, I_CONTROL, I_EVENT, I_WINDOW, I_AUDIO, I_VIDEO, I_POSITION, I_SEEKING, I_COUNT };

static const char *const unknown_m[] = {"QueryInterface", "AddRef", "Release"};
static const char *const dispatch_m[] = {"GetTypeInfoCount", "GetTypeInfo", "GetIDsOfNames", "Invoke"};
static const char *const graph_m[] = {
    "AddFilter", "RemoveFilter", "EnumFilters", "FindFilterByName", "ConnectDirect", "Reconnect", "Disconnect",
    "SetDefaultSyncSource", "Connect", "Render", "RenderFile", "AddSourceFilter", "SetLogFile", "Abort",
    "ShouldOperationContinue",
};
static const char *const control_m[] = {
    "Run", "Pause", "Stop", "GetState", "RenderFile", "AddSourceFilter", "get_FilterCollection",
    "get_RegFilterCollection", "StopWhenReady",
};
static const char *const event_m[] = {
    "GetEventHandle", "GetEvent", "WaitForCompletion", "CancelDefaultHandling", "RestoreDefaultHandling",
    "FreeEventParams", "SetNotifyWindow", "SetNotifyFlags", "GetNotifyFlags",
};
static const char *const window_m[] = {
    "put_Caption", "get_Caption", "put_WindowStyle", "get_WindowStyle", "put_WindowStyleEx",
    "get_WindowStyleEx", "put_AutoShow", "get_AutoShow", "put_WindowState", "get_WindowState",
    "put_BackgroundPalette", "get_BackgroundPalette", "put_Visible", "get_Visible", "put_Left", "get_Left",
    "put_Width", "get_Width", "put_Top", "get_Top", "put_Height", "get_Height", "put_Owner", "get_Owner",
    "put_MessageDrain", "get_MessageDrain", "get_BorderColor", "put_BorderColor", "get_FullScreenMode",
    "put_FullScreenMode", "SetWindowForeground", "NotifyOwnerMessage", "SetWindowPosition",
    "GetWindowPosition", "GetMinIdealImageSize", "GetMaxIdealImageSize", "GetRestorePosition",
    "HideCursor", "IsCursorHidden",
};
static const char *const audio_m[] = {"put_Volume", "get_Volume", "put_Balance", "get_Balance"};
static const char *const video_m[] = {
    "get_AvgTimePerFrame", "get_BitRate", "get_BitErrorRate", "get_VideoWidth", "get_VideoHeight",
    "put_SourceLeft", "get_SourceLeft", "put_SourceWidth", "get_SourceWidth", "put_SourceTop",
    "get_SourceTop", "put_SourceHeight", "get_SourceHeight", "put_DestinationLeft", "get_DestinationLeft",
    "put_DestinationWidth", "get_DestinationWidth", "put_DestinationTop", "get_DestinationTop",
    "put_DestinationHeight", "get_DestinationHeight", "SetSourcePosition", "GetSourcePosition",
    "SetDefaultSourcePosition", "SetDestinationPosition", "GetDestinationPosition",
    "SetDefaultDestinationPosition", "GetVideoSize", "GetVideoPaletteEntries", "GetCurrentImage",
    "IsUsingDefaultSource", "IsUsingDefaultDestination",
};
static const char *const position_m[] = {
    "get_Duration", "put_CurrentPosition", "get_CurrentPosition", "get_StopTime", "put_StopTime",
    "get_PrerollTime", "put_PrerollTime", "put_Rate", "get_Rate", "CanSeekForward", "CanSeekBackward",
};
static const char *const seeking_m[] = {
    "GetCapabilities", "CheckCapabilities", "IsFormatSupported", "QueryPreferredFormat", "GetTimeFormat",
    "IsUsingTimeFormat", "SetTimeFormat", "GetDuration", "GetStopPosition", "GetCurrentPosition",
    "ConvertTimeFormat", "SetPositions", "GetPositions", "GetAvailable", "SetRate", "GetRate", "GetPreroll",
};

static const struct {
    const char *name;
    bool dispatch;
    const char *const *m;
    uint32_t n;
} ifaces[I_COUNT] = {
    [I_GRAPH] = {"IGraphBuilder", false, graph_m, sizeof graph_m / sizeof *graph_m},
    [I_CONTROL] = {"IMediaControl", true, control_m, sizeof control_m / sizeof *control_m},
    [I_EVENT] = {"IMediaEventEx", true, event_m, sizeof event_m / sizeof *event_m},
    [I_WINDOW] = {"IVideoWindow", true, window_m, sizeof window_m / sizeof *window_m},
    [I_AUDIO] = {"IBasicAudio", true, audio_m, sizeof audio_m / sizeof *audio_m},
    [I_VIDEO] = {"IBasicVideo", true, video_m, sizeof video_m / sizeof *video_m},
    [I_POSITION] = {"IMediaPosition", true, position_m, sizeof position_m / sizeof *position_m},
    [I_SEEKING] = {"IMediaSeeking", false, seeking_m, sizeof seeking_m / sizeof *seeking_m},
};

/* The first dword of each interface's IID (all distinct). */
static const uint32_t iid_first[I_COUNT] = {
    [I_GRAPH] = 0x56a868a9u,    [I_CONTROL] = 0x56a868b1u, [I_EVENT] = 0x56a868c0u,
    [I_WINDOW] = 0x56a868b4u,   [I_AUDIO] = 0x56a868b3u,   [I_VIDEO] = 0x56a868b5u,
    [I_POSITION] = 0x56a868b2u, [I_SEEKING] = 0x36b73880u,
};

typedef struct graph {
    bool used;
    uint32_t refs;
    uint32_t iface[I_COUNT];
    plm_t *plm;
    bool running, done, reported;
    uint64_t started_ms;      /* clock when playback (re)started, less the time already played */
    rc_pcm *pcm;
    uint32_t *rgb;
    int w, h;
    uint32_t notify_hwnd, notify_msg, notify_param;
    int32_t volume;
    /* As a multimedia stream (amstream): the surface the frame being decoded goes to (a
     * guest interface pointer, 0: the window), and the video frames shown so far. */
    uint32_t target;
    uint32_t frames;
    bool new_frame;
    uint32_t video_stream, audio_stream;
} graph;

#define GRAPHS 8
static graph graphs[GRAPHS];
static uint32_t vtables[I_COUNT];
static bool hooked;

static uint32_t vtable(enum iface k)
{
    if (vtables[k]) {
        return vtables[k];
    }
    uint32_t n = 3 + (ifaces[k].dispatch ? 4 : 0) + ifaces[k].n;
    uint32_t t = rc_heap_alloc(4 * n, false);
    uint32_t j = 0;
    for (uint32_t i = 0; i < 3; i++, j++) {
        char buf[96];
        snprintf(buf, sizeof buf, "%s::%s", ifaces[k].name, unknown_m[i]);
        rc_st32(t + 4 * j, rc_import_address(COM, rc_str(rc_proc_str(buf))));
    }
    for (uint32_t i = 0; ifaces[k].dispatch && i < 4; i++, j++) {
        char buf[96];
        snprintf(buf, sizeof buf, "%s::%s", ifaces[k].name, dispatch_m[i]);
        rc_st32(t + 4 * j, rc_import_address(COM, rc_str(rc_proc_str(buf))));
    }
    for (uint32_t i = 0; i < ifaces[k].n; i++, j++) {
        char buf[96];
        snprintf(buf, sizeof buf, "%s::%s", ifaces[k].name, ifaces[k].m[i]);
        rc_st32(t + 4 * j, rc_import_address(COM, rc_str(rc_proc_str(buf))));
    }
    vtables[k] = t;
    return t;
}

static uint32_t iface_of(graph *g, enum iface k)
{
    if (!g->iface[k]) {
        uint32_t p = rc_heap_alloc(8, false);
        rc_st32(p, vtable(k));
        rc_st32(p + 4, (uint32_t)(g - graphs));
        g->iface[k] = p;
    }
    return g->iface[k];
}

static graph *self(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    uint32_t i = p ? rc_ld32(p + 4) : GRAPHS;
    if (i >= GRAPHS || !graphs[i].used) {
        rc_fatal("DirectShow: bad this pointer %#x", p);
    }
    return &graphs[i];
}

static void close_movie(graph *g)
{
    if (g->plm) {
        plm_destroy(g->plm);
        g->plm = NULL;
    }
    if (g->pcm) {
        rc_pcm_free(g->pcm);
        g->pcm = NULL;
    }
    free(g->rgb);
    g->rgb = NULL;
    g->running = false;
}

/* A frame into a DirectDraw surface, converted to its pixel format, from its top left. */
static void to_surface(graph *g, uint32_t surface)
{
    uint32_t o = rc_dd_object_of(surface, RC_DD_SURFACE);
    rc_dd_surface *d = o ? rc_dd_surf(o) : NULL;
    if (!d || !d->pixels || !g->rgb) {
        return;
    }
    uint32_t w = (uint32_t)g->w < d->w ? (uint32_t)g->w : d->w;
    uint32_t h = (uint32_t)g->h < d->h ? (uint32_t)g->h : d->h;
    uint32_t rm = d->rmask ? d->rmask : 0xf800, gm = d->gmask ? d->gmask : 0x07e0, bm = d->bmask ? d->bmask : 0x1f;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = rc_ptr(d->pixels + y * d->pitch);
        const uint32_t *src = g->rgb + (size_t)y * (size_t)g->w;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t v = src[x];
            if (d->bpp == 32) {
                memcpy(row + 4 * x, &v, 4);
            } else if (d->bpp == 24) {
                row[3 * x] = (uint8_t)v;
                row[3 * x + 1] = (uint8_t)(v >> 8);
                row[3 * x + 2] = (uint8_t)(v >> 16);
            } else if (d->bpp == 16) {
                /* Scale each 8-bit channel into its mask. */
                uint32_t r = (v >> 16) & 0xff, gr = (v >> 8) & 0xff, b = v & 0xff;
                uint32_t px = 0;
                const uint32_t masks[3] = {rm, gm, bm}, vals[3] = {r, gr, b};
                for (int k = 0; k < 3; k++) {
                    uint32_t m = masks[k];
                    if (!m) {
                        continue;
                    }
                    int sh = 0, bits = 0;
                    while (!((m >> sh) & 1)) {
                        sh++;
                    }
                    while ((m >> (sh + bits)) & 1) {
                        bits++;
                    }
                    px |= (vals[k] >> (8 - bits)) << sh;
                }
                uint16_t p16 = (uint16_t)px;
                memcpy(row + 2 * x, &p16, 2);
            }
        }
    }
}

static void on_video(plm_t *p, plm_frame_t *f, void *user)
{
    (void)p;
    graph *g = user;
    g->new_frame = true;
    g->frames++;
    if (!g->rgb || g->w != (int)f->width || g->h != (int)f->height) {
        free(g->rgb);
        g->w = (int)f->width;
        g->h = (int)f->height;
        g->rgb = malloc((size_t)g->w * (size_t)g->h * 4);
        if (!g->rgb) {
            return;
        }
    }
    /* BGRA bytes, little-endian: the platform's 0x00RRGGBB. */
    plm_frame_to_bgra(f, (uint8_t *)g->rgb, g->w * 4);
    if (g->target) {
        to_surface(g, g->target);
    } else {
        rc_plat_present(g->rgb, g->w, g->h, g->w);
    }
}

static void on_audio(plm_t *p, plm_samples_t *s, void *user)
{
    (void)p;
    graph *g = user;
    if (!g->pcm) {
        return;
    }
    int16_t pcm[PLM_AUDIO_SAMPLES_PER_FRAME * 2];
    for (unsigned i = 0; i < s->count * 2; i++) {
        float v = s->interleaved[i];
        v = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
        pcm[i] = (int16_t)(v * 32767.0f);
    }
    rc_pcm_put(g->pcm, pcm, s->count * 4);
}

/* Decode up to the clock; the end posts EC_COMPLETE. A multimedia stream decodes when its
 * samples are updated instead. */
static void advance(graph *g)
{
    if (!g->plm || !g->running || g->done || g->video_stream) {
        return;
    }
    double t = (double)(rc_ticks_ms() - g->started_ms) / 1000.0;
    double behind = t - plm_get_time(g->plm);
    if (behind > 0) {
        plm_decode(g->plm, behind > 0.5 ? 0.5 : behind);
    }
    if (plm_has_ended(g->plm)) {
        g->done = true;
        g->running = false;
        if (g->notify_hwnd) {
            rc_post_message(g->notify_hwnd, g->notify_msg, 0, g->notify_param);
        }
    }
}

static void advance_all(rc_cpu *c)
{
    (void)c;
    for (int i = 0; i < GRAPHS; i++) {
        if (graphs[i].used) {
            advance(&graphs[i]);
        }
    }
}

static uint32_t create(rc_cpu *c, uint32_t iid)
{
    (void)c;
    for (int i = 0; i < GRAPHS; i++) {
        if (!graphs[i].used) {
            graph *g = &graphs[i];
            memset(g, 0, sizeof *g);
            g->used = true;
            g->refs = 1;
            if (!hooked) {
                rc_add_pump_hook(advance_all);
                hooked = true;
            }
            for (int k = 0; k < I_COUNT; k++) {
                if (rc_ld32(iid) == iid_first[k]) {
                    return iface_of(g, (enum iface)k);
                }
            }
            return iface_of(g, I_GRAPH);
        }
    }
    return 0;
}

/* ---- IUnknown and IDispatch ---- */

static void u_QueryInterface(rc_cpu *c)
{
    graph *g = self(c);
    uint32_t iid = RC_ARG(c, 1), out = RC_ARG(c, 2);
    uint32_t first = rc_ld32(iid);
    for (int k = 0; k < I_COUNT; k++) {
        if (first == iid_first[k]) {
            g->refs++;
            rc_st32(out, iface_of(g, (enum iface)k));
            RC_RETURN(c, S_OK, 3);
        }
    }
    /* IUnknown {00000000-...} and IMediaEvent (0x56a868b6): the event interface covers it. */
    if (first == 0 || first == 0x56a868b6u) {
        g->refs++;
        rc_st32(out, first ? iface_of(g, I_EVENT) : iface_of(g, I_GRAPH));
        RC_RETURN(c, S_OK, 3);
    }
    rc_st32(out, 0);
    RC_RETURN(c, E_NOINTERFACE, 3);
}

static void u_AddRef(rc_cpu *c) { RC_RETURN(c, ++self(c)->refs, 1); }

static void u_Release(rc_cpu *c)
{
    graph *g = self(c);
    uint32_t r = g->refs ? --g->refs : 0;
    if (!r) {
        close_movie(g);
        g->used = false;
    }
    RC_RETURN(c, r, 1);
}

/* ---- IGraphBuilder ---- */

/* Open the movie at the guest's wide path `w`: S_OK or an error. */

/* pl_mpeg's file loader with the runtime's reads. */
static void load_movie(plm_buffer_t *self, void *user)
{
    (void)user;
    if (self->discard_read_bytes) {
        plm_buffer_discard_read_bytes(self);
    }
    size_t got = rc_host_fread(self->bytes + self->length, self->capacity - self->length, self->fh);
    self->length += got;
    if (got == 0) {
        self->has_ended = 1;
    }
}

static uint32_t open_movie(graph *g, uint32_t w)
{
    char path[RC_PATH_MAX], rel[RC_PATH_MAX], host[RC_PATH_MAX];
    size_t n = 0;
    for (; w && n + 1 < sizeof path; n++) {
        uint16_t ch = rc_ld16(w + 2 * (uint32_t)n);
        if (!ch) {
            break;
        }
        path[n] = ch < 0x80 ? (char)ch : '_';
    }
    path[n] = 0;
    close_movie(g);
    g->done = g->reported = false;
    g->frames = 0;
    if (!rc_k32_guest_rel(path, rel, sizeof rel) || !rc_k32_find_existing(rel, host, NULL, NULL)) {
        rc_log("DirectShow: \"%s\": not found", path);
        return VFW_E_NOT_FOUND;
    }
    /* The file read through rc_host_fread (in a browser the game's files are read only that way). */
    FILE *fh = fopen(host, "rb");
    plm_buffer_t *buf = fh ? plm_buffer_create_with_file(fh, 1) : NULL;
    if (buf) {
        plm_buffer_set_load_callback(buf, load_movie, NULL);
    }
    g->plm = buf ? plm_create_with_buffer(buf, 1) : NULL;
    if (!g->plm || !plm_probe(g->plm, 5000 * 1024)) {
        rc_log("DirectShow: \"%s\": not an MPEG-1 system stream", path);
        close_movie(g);
        return E_FAIL;
    }
    plm_set_video_decode_callback(g->plm, on_video, g);
    plm_set_audio_decode_callback(g->plm, on_audio, g);
    plm_set_loop(g->plm, 0);
    if (plm_get_num_audio_streams(g->plm) > 0) {
        plm_set_audio_enabled(g->plm, 1);
        int rate = plm_get_samplerate(g->plm);
        g->pcm = rc_audio_open(44100, 2) ? rc_pcm_open(rate, 2, 16) : NULL;
        /* Ahead of the picture by the stream's buffer, as a sound card plays it. */
        plm_set_audio_lead_time(g->plm, 0.1);
    }
    return S_OK;
}

/* RenderFile(this, wide path, playlist) */
static void g_RenderFile(rc_cpu *c) { RC_RETURN(c, open_movie(self(c), RC_ARG(c, 1)), 3); }

/* ---- IMediaControl ---- */

static void m_Run(rc_cpu *c)
{
    graph *g = self(c);
    if (g->plm && !g->running && !g->done) {
        g->started_ms = rc_ticks_ms() - (uint64_t)(plm_get_time(g->plm) * 1000.0);
        g->running = true;
        if (g->pcm) {
            rc_pcm_pause(g->pcm, false);
        }
    }
    RC_RETURN(c, S_OK, 1);
}

static void m_Pause(rc_cpu *c)
{
    graph *g = self(c);
    g->running = false;
    if (g->pcm) {
        rc_pcm_pause(g->pcm, true);
    }
    RC_RETURN(c, S_OK, 1);
}

static void m_Stop(rc_cpu *c)
{
    graph *g = self(c);
    g->running = false;
    if (g->pcm) {
        rc_pcm_clear(g->pcm);
    }
    if (g->plm) {
        plm_rewind(g->plm);
    }
    RC_RETURN(c, S_OK, 1);
}

/* GetState(this, timeout, state*): 0 stopped, 1 paused, 2 running. */
static void m_GetState(rc_cpu *c)
{
    graph *g = self(c);
    advance(g);
    rc_st32(RC_ARG(c, 2), g->running ? 2 : 0);
    RC_RETURN(c, S_OK, 3);
}

/* ---- IMediaEventEx ---- */

/* GetEvent(this, code*, param1*, param2*, timeout) */
static void e_GetEvent(rc_cpu *c)
{
    graph *g = self(c);
    advance(g);
    if (g->done && !g->reported) {
        g->reported = true;
        rc_st32(RC_ARG(c, 1), EC_COMPLETE);
        rc_st32(RC_ARG(c, 2), 0);
        rc_st32(RC_ARG(c, 3), 0);
        RC_RETURN(c, S_OK, 5);
    }
    RC_RETURN(c, E_ABORT, 5);
}

/* WaitForCompletion(this, timeout, code*) */
static void e_WaitForCompletion(rc_cpu *c)
{
    graph *g = self(c);
    uint32_t timeout = RC_ARG(c, 1);
    uint64_t until = rc_ticks_ms() + (timeout == 0xffffffffu ? UINT64_C(1) << 40 : timeout);
    while (g->running && !g->done && rc_ticks_ms() < until) {
        rc_pump_events();
        advance(g);
        rc_sleep_ms(5);
    }
    rc_st32(RC_ARG(c, 2), g->done ? EC_COMPLETE : 0);
    RC_RETURN(c, g->done ? S_OK : E_ABORT, 3);
}

static void e_GetEventHandle(rc_cpu *c)
{
    rc_st32(RC_ARG(c, 1), 0);
    RC_RETURN(c, S_OK, 2);
}

/* SetNotifyWindow(this, hwnd, message, param) */
static void e_SetNotifyWindow(rc_cpu *c)
{
    graph *g = self(c);
    g->notify_hwnd = RC_ARG(c, 1);
    g->notify_msg = RC_ARG(c, 2);
    g->notify_param = RC_ARG(c, 3);
    RC_RETURN(c, S_OK, 4);
}

static void e_GetNotifyFlags(rc_cpu *c)
{
    rc_st32(RC_ARG(c, 1), 0);
    RC_RETURN(c, S_OK, 2);
}

/* ---- the rest: settings with no effect on how the movie is shown ---- */

static void ok1(rc_cpu *c) { RC_RETURN(c, S_OK, 1); }
static void ok2(rc_cpu *c) { RC_RETURN(c, S_OK, 2); }
static void ok3(rc_cpu *c) { RC_RETURN(c, S_OK, 3); }
static void ok4(rc_cpu *c) { RC_RETURN(c, S_OK, 4); }
static void ok5(rc_cpu *c) { RC_RETURN(c, S_OK, 5); }

static void put_zero2(rc_cpu *c)
{
    if (RC_ARG(c, 1)) {
        rc_st32(RC_ARG(c, 1), 0);
    }
    RC_RETURN(c, S_OK, 2);
}

static void a_put_Volume(rc_cpu *c)
{
    graph *g = self(c);
    g->volume = (int32_t)RC_ARG(c, 1);
    if (g->pcm) {
        rc_pcm_gain(g->pcm, g->volume <= -10000 ? 0.0f : powf(10.0f, (float)g->volume / 2000.0f));
    }
    RC_RETURN(c, S_OK, 2);
}

static void a_get_Volume(rc_cpu *c)
{
    rc_st32(RC_ARG(c, 1), (uint32_t)self(c)->volume);
    RC_RETURN(c, S_OK, 2);
}

/* get_Duration(this, double*) and get_CurrentPosition: seconds. */
static void p_put_double(rc_cpu *c, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, 8);
    rc_st32(RC_ARG(c, 1), (uint32_t)bits);
    rc_st32(RC_ARG(c, 1) + 4, (uint32_t)(bits >> 32));
}
static void p_get_Duration(rc_cpu *c)
{
    graph *g = self(c);
    p_put_double(c, g->plm ? plm_get_duration(g->plm) : 0.0);
    RC_RETURN(c, S_OK, 2);
}
static void p_get_CurrentPosition(rc_cpu *c)
{
    graph *g = self(c);
    advance(g);
    p_put_double(c, g->plm ? plm_get_time(g->plm) : 0.0);
    RC_RETURN(c, S_OK, 2);
}

/* IBasicVideo get_VideoWidth/Height and GetVideoSize. */
static void v_get_VideoWidth(rc_cpu *c)
{
    graph *g = self(c);
    rc_st32(RC_ARG(c, 1), g->plm ? (uint32_t)plm_get_width(g->plm) : 0);
    RC_RETURN(c, S_OK, 2);
}
static void v_get_VideoHeight(rc_cpu *c)
{
    graph *g = self(c);
    rc_st32(RC_ARG(c, 1), g->plm ? (uint32_t)plm_get_height(g->plm) : 0);
    RC_RETURN(c, S_OK, 2);
}

/* ---- the multimedia stream (amstream): frames into DirectDraw surfaces ---- */

#define MS_S_ENDOFSTREAM 0x40003u
#define MS_E_NOSTREAM 0x80040410u
enum { A_STREAM, A_MEDIA, A_SAMPLE, A_KINDS };
static const char *const ms_m[] = {
    "QueryInterface", "AddRef", "Release", "GetInformation", "GetMediaStream", "EnumMediaStreams", "GetState",
    "SetState", "GetTime", "GetDuration", "Seek", "GetEndOfStreamEventHandle", "Initialize", "GetFilterGraph",
    "GetFilter", "AddMediaStream", "OpenFile", "OpenMoniker", "Render",
};
static const char *const media_m[] = {
    "QueryInterface", "AddRef", "Release", "GetMultiMediaStream", "GetInformation", "SetSameFormat",
    "AllocateSample", "CreateSharedSample", "SendEndOfStream", "GetFormat", "SetFormat", "GetDirectDraw",
    "SetDirectDraw", "CreateSample", "GetTimePerFrame",
};
static const char *const sample_m[] = {
    "QueryInterface", "AddRef", "Release", "GetMediaStream", "GetSampleTimes", "SetSampleTimes", "Update",
    "CompletionStatus", "GetSurface", "SetRect",
};

/* A guest block: vtable, graph index, kind, and (a sample) its surface. */
static uint32_t am_object(graph *g, int kind, uint32_t surface)
{
    static uint32_t vt[A_KINDS];
    if (!vt[kind]) {
        const char *const *m = kind == A_STREAM ? ms_m : kind == A_MEDIA ? media_m : sample_m;
        const char *name = kind == A_STREAM ? "IAMMultiMediaStream" : kind == A_MEDIA ? "IDirectDrawMediaStream"
                                                                                      : "IDirectDrawStreamSample";
        uint32_t n = kind == A_STREAM ? sizeof ms_m / sizeof *ms_m
                   : kind == A_MEDIA ? sizeof media_m / sizeof *media_m
                                     : sizeof sample_m / sizeof *sample_m;
        vt[kind] = rc_heap_alloc(4 * n, false);
        for (uint32_t i = 0; i < n; i++) {
            char buf[96];
            snprintf(buf, sizeof buf, "%s::%s", name, m[i]);
            rc_st32(vt[kind] + 4 * i, rc_import_address(COM, rc_str(rc_proc_str(buf))));
        }
    }
    g->refs++; /* each interface handed out holds a reference */
    uint32_t p = rc_heap_alloc(16, true);
    rc_st32(p, vt[kind]);
    rc_st32(p + 4, (uint32_t)(g - graphs));
    rc_st32(p + 8, (uint32_t)kind);
    rc_st32(p + 12, surface);
    return p;
}

static uint32_t create_ms(rc_cpu *c, uint32_t iid)
{
    (void)c;
    (void)iid;
    for (int i = 0; i < GRAPHS; i++) {
        if (!graphs[i].used) {
            graph *g = &graphs[i];
            memset(g, 0, sizeof *g);
            g->used = true;
            g->refs = 0;
            return am_object(g, A_STREAM, 0);
        }
    }
    return 0;
}

static void am_QueryInterface(rc_cpu *c)
{
    self(c)->refs++;
    rc_st32(RC_ARG(c, 2), RC_ARG(c, 0));
    RC_RETURN(c, S_OK, 3);
}

/* AddMediaStream(this, object, purpose, flags, new stream*): video makes the stream that
 * samples come from; audio with the default renderer plays through the platform. */
static void am_AddMediaStream(rc_cpu *c)
{
    graph *g = self(c);
    uint32_t purpose = RC_ARG(c, 2), out = RC_ARG(c, 4);
    uint32_t first = rc_ld32(purpose);
    uint32_t st = am_object(g, A_MEDIA, 0);
    if (first == 0xa35ff56au) { /* MSPID_PrimaryVideo */
        g->video_stream = st;
    } else {
        g->audio_stream = st;
    }
    if (out) {
        rc_st32(out, st);
    }
    RC_RETURN(c, S_OK, 5);
}

/* OpenFile(this, wide path, flags) */
static void am_OpenFile(rc_cpu *c)
{
    graph *g = self(c);
    uint32_t video = g->video_stream, audio = g->audio_stream;
    uint32_t r = open_movie(g, RC_ARG(c, 1));
    g->video_stream = video;
    g->audio_stream = audio;
    if (g->plm && !audio && g->pcm) {
        /* No audio stream asked for: the movie stays silent. */
        rc_pcm_free(g->pcm);
        g->pcm = NULL;
    }
    RC_RETURN(c, r, 3);
}

/* GetMediaStream(this, purpose, stream*) */
static void am_GetMediaStream(rc_cpu *c)
{
    graph *g = self(c);
    uint32_t first = rc_ld32(RC_ARG(c, 1));
    uint32_t st = first == 0xa35ff56au ? g->video_stream : g->audio_stream;
    rc_st32(RC_ARG(c, 2), st);
    RC_RETURN(c, st ? S_OK : MS_E_NOSTREAM, 3);
}

/* SetState(this, state): 1 run, 0 stop. */
static void am_SetState(rc_cpu *c)
{
    graph *g = self(c);
    bool run = RC_ARG(c, 1) == 1;
    if (run && g->plm && !g->running) {
        g->started_ms = rc_ticks_ms() - (uint64_t)(plm_get_time(g->plm) * 1000.0);
        g->running = true;
        if (g->pcm) {
            rc_pcm_pause(g->pcm, false);
        }
    } else if (!run) {
        g->running = false;
        if (g->pcm) {
            rc_pcm_pause(g->pcm, true);
        }
    }
    RC_RETURN(c, S_OK, 2);
}

static void am_GetState(rc_cpu *c)
{
    rc_st32(RC_ARG(c, 1), self(c)->running ? 1 : 0);
    RC_RETURN(c, S_OK, 2);
}

/* GetTime / GetDuration(this, STREAM_TIME*): 100 ns units. */
static void put64(uint32_t p, uint64_t v)
{
    rc_st32(p, (uint32_t)v);
    rc_st32(p + 4, (uint32_t)(v >> 32));
}
static void am_GetTime(rc_cpu *c)
{
    graph *g = self(c);
    put64(RC_ARG(c, 1), g->plm ? (uint64_t)(plm_get_time(g->plm) * 1e7) : 0);
    RC_RETURN(c, S_OK, 2);
}
static void am_GetDuration(rc_cpu *c)
{
    graph *g = self(c);
    put64(RC_ARG(c, 1), g->plm ? (uint64_t)(plm_get_duration(g->plm) * 1e7) : 0);
    RC_RETURN(c, S_OK, 2);
}

/* Seek(this, STREAM_TIME): only back to the start. */
static void am_Seek(rc_cpu *c)
{
    graph *g = self(c);
    if (g->plm && RC_ARG(c, 1) == 0 && RC_ARG(c, 2) == 0) {
        plm_rewind(g->plm);
        g->done = false;
        g->frames = 0;
        g->started_ms = rc_ticks_ms();
    }
    RC_RETURN(c, S_OK, 3);
}

/* GetFormat(this, current DDSURFACEDESC*, palette*, desired*, flags*): the movie's size. */
static void md_GetFormat(rc_cpu *c)
{
    graph *g = self(c);
    for (int k = 1; k <= 3; k += 2) {
        uint32_t d = RC_ARG(c, k);
        if (d && g->plm) {
            rc_st32(d + 4, rc_ld32(d + 4) | 0x6u); /* DDSD_HEIGHT | DDSD_WIDTH */
            rc_st32(d + 8, (uint32_t)plm_get_height(g->plm));
            rc_st32(d + 12, (uint32_t)plm_get_width(g->plm));
        }
    }
    if (RC_ARG(c, 2)) {
        rc_st32(RC_ARG(c, 2), 0);
    }
    if (RC_ARG(c, 4)) {
        rc_st32(RC_ARG(c, 4), 0);
    }
    RC_RETURN(c, S_OK, 5);
}

/* CreateSample(this, surface, rect, flags, sample*) */
static void md_CreateSample(rc_cpu *c)
{
    graph *g = self(c);
    uint32_t surface = RC_ARG(c, 1);
    if (!surface) {
        /* None given: the stream makes one of the movie's size, which the program gets
         * with GetSurface. */
        surface = rc_dd_offscreen(g->plm ? (uint32_t)plm_get_width(g->plm) : 320,
                                  g->plm ? (uint32_t)plm_get_height(g->plm) : 240);
    }
    rc_st32(RC_ARG(c, 4), am_object(g, A_SAMPLE, surface));
    RC_RETURN(c, S_OK, 5);
}

/* GetTimePerFrame(this, STREAM_TIME*) */
static void md_GetTimePerFrame(rc_cpu *c)
{
    graph *g = self(c);
    double fps = g->plm ? plm_get_framerate(g->plm) : 25.0;
    put64(RC_ARG(c, 1), (uint64_t)(1e7 / (fps > 0 ? fps : 25.0)));
    RC_RETURN(c, S_OK, 2);
}

/* Update(this, flags, event, apc, context): the next frame, when it is due, into the surface. */
static void sm_Update(rc_cpu *c)
{
    graph *g = self(c);
    uint32_t surface = rc_ld32(RC_ARG(c, 0) + 12);
    if (!g->plm || g->done) {
        RC_RETURN(c, MS_S_ENDOFSTREAM, 5);
    }
    if (!g->running) {
        g->started_ms = rc_ticks_ms() - (uint64_t)(plm_get_time(g->plm) * 1000.0);
        g->running = true;
    }
    double fps = plm_get_framerate(g->plm);
    double step = 1.0 / (fps > 0 ? fps : 25.0);
    /* Wait for the frame's time, then decode until a picture comes (with its sound). */
    uint64_t due = g->started_ms + (uint64_t)((double)g->frames * step * 1000.0);
    uint64_t now = rc_ticks_ms();
    if (due > now) {
        rc_sleep_ms((uint32_t)(due - now));
    }
    g->target = surface;
    g->new_frame = false;
    for (int i = 0; i < 16 && !g->new_frame && !plm_has_ended(g->plm); i++) {
        plm_decode(g->plm, step);
    }
    g->target = 0;
    if (!g->new_frame) {
        g->done = true;
        RC_RETURN(c, MS_S_ENDOFSTREAM, 5);
    }
    RC_RETURN(c, S_OK, 5);
}

static void sm_GetSurface(rc_cpu *c)
{
    uint32_t surface = rc_ld32(RC_ARG(c, 0) + 12);
    if (RC_ARG(c, 1)) {
        rc_st32(RC_ARG(c, 1), surface);
    }
    if (RC_ARG(c, 2)) {
        graph *g = self(c);
        uint32_t r = RC_ARG(c, 2);
        rc_st32(r, 0);
        rc_st32(r + 4, 0);
        rc_st32(r + 8, g->plm ? (uint32_t)plm_get_width(g->plm) : 0);
        rc_st32(r + 12, g->plm ? (uint32_t)plm_get_height(g->plm) : 0);
    }
    RC_RETURN(c, S_OK, 3);
}

/* CompletionStatus(this, flags, timeout): the update is always complete. */
static void sm_CompletionStatus(rc_cpu *c) { RC_RETURN(c, self(c)->done ? MS_S_ENDOFSTREAM : S_OK, 3); }

#define AM(m, fn)                                                                                  \
    {COM, "IAMMultiMediaStream::" m, fn}, {COM, "IDirectDrawMediaStream::" m, fn},                \
        {COM, "IDirectDrawStreamSample::" m, fn}

static const rc_import_def am_defs[] = {
    AM("QueryInterface", am_QueryInterface),
    AM("AddRef", u_AddRef),
    AM("Release", u_Release),
    {COM, "IAMMultiMediaStream::Initialize", ok4},
    {COM, "IAMMultiMediaStream::AddMediaStream", am_AddMediaStream},
    {COM, "IAMMultiMediaStream::OpenFile", am_OpenFile},
    {COM, "IAMMultiMediaStream::GetMediaStream", am_GetMediaStream},
    {COM, "IAMMultiMediaStream::SetState", am_SetState},
    {COM, "IAMMultiMediaStream::GetState", am_GetState},
    {COM, "IAMMultiMediaStream::GetTime", am_GetTime},
    {COM, "IAMMultiMediaStream::GetDuration", am_GetDuration},
    {COM, "IAMMultiMediaStream::Seek", am_Seek},
    {COM, "IDirectDrawMediaStream::GetFormat", md_GetFormat},
    {COM, "IDirectDrawMediaStream::SetFormat", ok3},
    {COM, "IDirectDrawMediaStream::SetDirectDraw", ok2},
    {COM, "IDirectDrawMediaStream::CreateSample", md_CreateSample},
    {COM, "IDirectDrawMediaStream::GetTimePerFrame", md_GetTimePerFrame},
    {COM, "IDirectDrawStreamSample::Update", sm_Update},
    {COM, "IDirectDrawStreamSample::GetSurface", sm_GetSurface},
    {COM, "IDirectDrawStreamSample::SetRect", ok2},
    {COM, "IDirectDrawStreamSample::CompletionStatus", sm_CompletionStatus},
};

#define ALL(m, fn)                                                                                 \
    {COM, "IGraphBuilder::" m, fn}, {COM, "IMediaControl::" m, fn}, {COM, "IMediaEventEx::" m, fn}, \
        {COM, "IVideoWindow::" m, fn}, {COM, "IBasicAudio::" m, fn}, {COM, "IBasicVideo::" m, fn}, \
        {COM, "IMediaPosition::" m, fn}, {COM, "IMediaSeeking::" m, fn}

static const rc_import_def defs[] = {
    ALL("QueryInterface", u_QueryInterface),
    ALL("AddRef", u_AddRef),
    ALL("Release", u_Release),
    {COM, "IGraphBuilder::RenderFile", g_RenderFile},
    {COM, "IGraphBuilder::SetLogFile", ok2},
    {COM, "IGraphBuilder::Abort", ok1},
    {COM, "IMediaControl::Run", m_Run},
    {COM, "IMediaControl::Pause", m_Pause},
    {COM, "IMediaControl::Stop", m_Stop},
    {COM, "IMediaControl::StopWhenReady", m_Stop},
    {COM, "IMediaControl::GetState", m_GetState},
    {COM, "IMediaEventEx::GetEvent", e_GetEvent},
    {COM, "IMediaEventEx::WaitForCompletion", e_WaitForCompletion},
    {COM, "IMediaEventEx::GetEventHandle", e_GetEventHandle},
    {COM, "IMediaEventEx::FreeEventParams", ok4},
    {COM, "IMediaEventEx::SetNotifyWindow", e_SetNotifyWindow},
    {COM, "IMediaEventEx::SetNotifyFlags", ok2},
    {COM, "IMediaEventEx::GetNotifyFlags", e_GetNotifyFlags},
    {COM, "IMediaEventEx::CancelDefaultHandling", ok2},
    {COM, "IMediaEventEx::RestoreDefaultHandling", ok2},
    /* The picture always fills the window: where the program puts its video window, and how,
     * changes nothing. */
    {COM, "IVideoWindow::put_Owner", ok2},
    {COM, "IVideoWindow::put_WindowStyle", ok2},
    {COM, "IVideoWindow::put_WindowStyleEx", ok2},
    {COM, "IVideoWindow::put_Visible", ok2},
    {COM, "IVideoWindow::put_AutoShow", ok2},
    {COM, "IVideoWindow::put_MessageDrain", ok2},
    {COM, "IVideoWindow::put_FullScreenMode", ok2},
    {COM, "IVideoWindow::put_WindowState", ok2},
    {COM, "IVideoWindow::put_BackgroundPalette", ok2},
    {COM, "IVideoWindow::put_BorderColor", ok2},
    {COM, "IVideoWindow::put_Left", ok2},
    {COM, "IVideoWindow::put_Top", ok2},
    {COM, "IVideoWindow::put_Width", ok2},
    {COM, "IVideoWindow::put_Height", ok2},
    {COM, "IVideoWindow::get_Visible", put_zero2},
    {COM, "IVideoWindow::get_FullScreenMode", put_zero2},
    {COM, "IVideoWindow::SetWindowPosition", ok5},
    {COM, "IVideoWindow::SetWindowForeground", ok2},
    {COM, "IVideoWindow::HideCursor", ok2},
    {COM, "IVideoWindow::NotifyOwnerMessage", ok5},
    {COM, "IBasicAudio::put_Volume", a_put_Volume},
    {COM, "IBasicAudio::get_Volume", a_get_Volume},
    {COM, "IBasicAudio::put_Balance", ok2},
    {COM, "IBasicVideo::get_VideoWidth", v_get_VideoWidth},
    {COM, "IBasicVideo::get_VideoHeight", v_get_VideoHeight},
    {COM, "IBasicVideo::SetDestinationPosition", ok5},
    {COM, "IBasicVideo::SetSourcePosition", ok5},
    {COM, "IBasicVideo::SetDefaultDestinationPosition", ok1},
    {COM, "IBasicVideo::SetDefaultSourcePosition", ok1},
    {COM, "IMediaPosition::get_Duration", p_get_Duration},
    {COM, "IMediaPosition::get_CurrentPosition", p_get_CurrentPosition},
    {COM, "IMediaPosition::put_CurrentPosition", ok3},
    {COM, "IMediaSeeking::SetTimeFormat", ok2},
    {COM, "IMediaSeeking::SetPositions", ok5},
    {COM, "IMediaSeeking::SetRate", ok3},
    {COM, "IGraphBuilder::AddFilter", ok3},
    {COM, "IGraphBuilder::Render", ok2},
    {COM, "IMediaPosition::put_Rate", ok3},
    {COM, "IBasicVideo::GetVideoSize", ok3},
    {COM, "IMediaSeeking::GetCapabilities", put_zero2},
};

void rc_dshow_init(void)
{
    rc_register_imports(defs, sizeof defs / sizeof defs[0]);
    rc_com_register("{E436EBB3-524F-11CE-9F53-0020AF0BA770}", create); /* CLSID_FilterGraph */
    rc_register_imports(am_defs, sizeof am_defs / sizeof am_defs[0]);
    rc_com_register("{49C47CE5-9BA4-11D0-8212-00C04FC32C45}", create_ms); /* CLSID_AMMultiMediaStream */
}
