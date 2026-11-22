/* SPDX-License-Identifier: MIT */
/*
 * DirectInput (version 3 to 7 interfaces, the A variants, DirectInputCreateEx): the system keyboard
 * and mouse.
 *
 * The devices follow the input the program gets through user32 (rc_add_input_observer):
 * the keyboard as 256 key states by DirectInput key code (the PC scan code, with 0x80 for
 * the extended keys), the mouse as relative motion, wheel and four buttons. Both give their
 * state (GetDeviceState) and, once a buffer size is set, the changes in order
 * (GetDeviceData). Other devices (joysticks) are not there.
 *
 * COM objects are as in ddraw.c: a guest block with the vtable and the host object's index;
 * the vtable holds reserved import addresses named after the methods.
 */
#include "dinput.h"

#include <stdio.h>
#include <string.h>

#include "rc/hle_modules.h"
#include "rc/log.h"
#include "rc/platform.h"
#include "rc/process.h"
#include "rc/vm.h"
#include "user32/user32.h"

#define DINPUT "DINPUT.dll"
#define COM "DINPUT_COM"

#define DI_OK 0u
#define DI_BUFFEROVERFLOW 1u
#define E_NOINTERFACE 0x80004002u
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_NOTACQUIRED 0x8007000cu
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_UNSUPPORTED 0x80004001u

#define DIPROP_BUFFERSIZE 1u
#define DIGDD_PEEK 1u

enum kind { K_FREE, K_DI, K_KEYBOARD, K_MOUSE };

#define EVENTS 256
typedef struct devdata {
    uint32_t ofs, data, time, seq;
} devdata;

typedef struct object {
    enum kind kind;
    uint32_t refs;
    uint32_t iface;
    bool acquired;
    uint32_t buffer; /* buffered events the program asked for, 0: none */
    devdata ev[EVENTS];
    uint32_t head, tail;
    bool overflow;
    uint32_t data_size; /* of the data format set */
} object;

#define OBJECTS 16
static object objects[OBJECTS];
static uint8_t keyboard[256];
static int32_t mouse_dx, mouse_dy, mouse_dz;
static uint8_t buttons[4];
static int last_x = -1, last_y = -1;
static uint32_t sequence;
static bool observing;

static const char *const di_methods[] = {
    "QueryInterface", "AddRef", "Release", "CreateDevice", "EnumDevices", "GetDeviceStatus",
    "RunControlPanel", "Initialize", "FindDevice", "CreateDeviceEx",
};
static const char *const dev_methods[] = {
    "QueryInterface", "AddRef", "Release", "GetCapabilities", "EnumObjects", "GetProperty", "SetProperty",
    "Acquire", "Unacquire", "GetDeviceState", "GetDeviceData", "SetDataFormat", "SetEventNotification",
    "SetCooperativeLevel", "GetObjectInfo", "GetDeviceInfo", "RunControlPanel", "Initialize",
    "CreateEffect", "EnumEffects", "GetEffectInfo", "GetForceFeedbackState", "SendForceFeedbackCommand",
    "EnumCreatedEffectObjects", "Escape", "Poll", "SendDeviceData", "EnumEffectsInFile",
    "WriteEffectToFile",
};

static uint32_t vtable(bool device)
{
    static uint32_t tables[2];
    uint32_t *t = &tables[device];
    if (*t) {
        return *t;
    }
    const char *const *m = device ? dev_methods : di_methods;
    uint32_t n = device ? sizeof dev_methods / sizeof *dev_methods : sizeof di_methods / sizeof *di_methods;
    *t = rc_heap_alloc(4 * n, false);
    for (uint32_t i = 0; i < n; i++) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s::%s", device ? "IDirectInputDevice" : "IDirectInput", m[i]);
        rc_st32(*t + 4 * i, rc_import_address(COM, rc_str(rc_proc_str(buf))));
    }
    return *t;
}

static uint32_t new_object(enum kind k)
{
    for (uint32_t i = 1; i < OBJECTS; i++) {
        if (objects[i].kind == K_FREE) {
            memset(&objects[i], 0, sizeof objects[i]);
            objects[i].kind = k;
            objects[i].refs = 1;
            uint32_t g = rc_heap_alloc(8, false);
            rc_st32(g, vtable(k != K_DI));
            rc_st32(g + 4, i);
            objects[i].iface = g;
            return g;
        }
    }
    rc_fatal("DirectInput: out of objects");
}

static object *self(rc_cpu *c)
{
    uint32_t g = RC_ARG(c, 0);
    uint32_t o = g ? rc_ld32(g + 4) : 0;
    if (!o || o >= OBJECTS || objects[o].kind == K_FREE) {
        rc_fatal("DirectInput: bad this pointer %#x", g);
    }
    return &objects[o];
}

/* A change for every device of `kind` that buffers. */
static void push(enum kind kind, uint32_t ofs, uint32_t data)
{
    uint32_t now = (uint32_t)rc_ticks_ms();
    sequence++;
    for (uint32_t i = 1; i < OBJECTS; i++) {
        object *o = &objects[i];
        if (o->kind != kind || !o->buffer || !o->acquired) {
            continue;
        }
        if (o->tail - o->head >= o->buffer || o->tail - o->head >= EVENTS) {
            o->overflow = true;
            continue;
        }
        o->ev[o->tail % EVENTS] = (devdata){ofs, data, now, sequence};
        o->tail++;
    }
}

static void observe(const rc_event *e)
{
    switch (e->type) {
    case RC_EV_KEY: {
        uint32_t dik = (uint32_t)(e->scancode & 0x7f) | (e->extended ? 0x80u : 0);
        if (!dik || e->repeat) {
            break;
        }
        uint8_t v = e->down ? 0x80 : 0;
        if (keyboard[dik] != v) {
            keyboard[dik] = v;
            push(K_KEYBOARD, dik, v);
        }
        break;
    }
    case RC_EV_MOUSE_MOVE:
        if (e->has_motion || last_x >= 0) {
            /* The mouse's own motion where the host has it (it goes on at the window's edge and
             * is not scaled with the picture), else how far the pointer moved (scripts). */
            int dx = e->has_motion ? e->dx : e->x - last_x, dy = e->has_motion ? e->dy : e->y - last_y;
            mouse_dx += dx;
            mouse_dy += dy;
            if (dx) {
                push(K_MOUSE, 0, (uint32_t)dx);
            }
            if (dy) {
                push(K_MOUSE, 4, (uint32_t)dy);
            }
        }
        last_x = e->x;
        last_y = e->y;
        break;
    case RC_EV_MOUSE_BUTTON: {
        int b = e->button == 1 ? 0 : e->button == 2 ? 1 : e->button == 3 ? 2 : 3;
        buttons[b] = e->down ? 0x80 : 0;
        push(K_MOUSE, 12u + (uint32_t)b, buttons[b]);
        break;
    }
    case RC_EV_WHEEL:
        mouse_dz += e->delta;
        push(K_MOUSE, 8, (uint32_t)e->delta);
        break;
    case RC_EV_FOCUS:
        if (!e->down) {
            memset(keyboard, 0, sizeof keyboard);
            memset(buttons, 0, sizeof buttons);
        }
        break;
    default:
        break;
    }
}

/* The window catches the mouse while the program reads it here and shows its picture through
 * DirectDraw (a full-screen game, which draws its own pointer from the mouse's motion): on
 * Windows that motion goes on at the screen's edge and the pointer never leaves the game. */
static void update_capture(void)
{
    static bool captured;
    bool want = false;
    for (uint32_t i = 1; i < OBJECTS && !want; i++) {
        want = objects[i].kind == K_MOUSE && objects[i].acquired;
    }
    want = want && rc_ddraw_has_primary();
    if (want != captured) {
        captured = want;
        rc_plat_mouse_capture(want);
    }
}

/* ---- IDirectInput ---- */

/* DirectInputCreateA(instance, version, result, outer) */
static void k_DirectInputCreateA(rc_cpu *c)
{
    if (!observing) {
        rc_add_input_observer(observe);
        observing = true;
    }
    rc_st32(RC_ARG(c, 2), new_object(K_DI));
    RC_RETURN(c, DI_OK, 4);
}

/* DirectInputCreateEx(instance, version, riid, result, outer): IDirectInput7 (whose layout begins
 * with the older interfaces'). */
static void k_DirectInputCreateEx(rc_cpu *c)
{
    if (!observing) {
        rc_add_input_observer(observe);
        observing = true;
    }
    rc_st32(RC_ARG(c, 3), new_object(K_DI));
    RC_RETURN(c, DI_OK, 5);
}

static void m_QueryInterface(rc_cpu *c)
{
    /* Every interface version shares one vtable layout prefix: hand out the same object. */
    object *o = self(c);
    o->refs++;
    rc_st32(RC_ARG(c, 2), RC_ARG(c, 0));
    RC_RETURN(c, DI_OK, 3);
}

static void m_AddRef(rc_cpu *c)
{
    object *o = self(c);
    RC_RETURN(c, ++o->refs, 1);
}

static void m_Release(rc_cpu *c)
{
    object *o = self(c);
    uint32_t r = o->refs ? --o->refs : 0;
    if (!r) {
        o->kind = K_FREE;
        update_capture();
    }
    RC_RETURN(c, r, 1);
}

/* CreateDevice(this, guid, result, outer): GUID_SysKeyboard and GUID_SysMouse. */
static void m_CreateDevice(rc_cpu *c)
{
    uint32_t guid = RC_ARG(c, 1);
    uint32_t d1 = rc_ld32(guid);
    bool sys = rc_ld16(guid + 4) == 0xd5a0 && rc_ld16(guid + 6) == 0x11cf;
    enum kind k = sys && d1 == 0x6f1d2b61u ? K_KEYBOARD : sys && d1 == 0x6f1d2b60u ? K_MOUSE : K_FREE;
    if (k == K_FREE) {
        rc_st32(RC_ARG(c, 2), 0);
        RC_RETURN(c, DIERR_DEVICENOTREG, 4);
    }
    rc_st32(RC_ARG(c, 2), new_object(k));
    RC_RETURN(c, DI_OK, 4);
}

/* CreateDeviceEx(this, guid, riid, result, outer): IDirectInputDevice7, the same object. */
static void m_CreateDeviceEx(rc_cpu *c)
{
    uint32_t guid = RC_ARG(c, 1);
    uint32_t d1 = rc_ld32(guid);
    bool sys = rc_ld16(guid + 4) == 0xd5a0 && rc_ld16(guid + 6) == 0x11cf;
    enum kind k = sys && d1 == 0x6f1d2b61u ? K_KEYBOARD : sys && d1 == 0x6f1d2b60u ? K_MOUSE : K_FREE;
    if (k == K_FREE) {
        rc_st32(RC_ARG(c, 3), 0);
        RC_RETURN(c, DIERR_DEVICENOTREG, 5);
    }
    rc_st32(RC_ARG(c, 3), new_object(k));
    RC_RETURN(c, DI_OK, 5);
}

/* EnumDevices(this, type, callback, ref, flags): the keyboard and mouse are not listed;
 * programs create them by their GUIDs. */
static void m_EnumDevices(rc_cpu *c) { RC_RETURN(c, DI_OK, 5); }
static void m_GetDeviceStatus(rc_cpu *c)
{
    uint32_t d1 = rc_ld32(RC_ARG(c, 1));
    RC_RETURN(c, d1 == 0x6f1d2b61u || d1 == 0x6f1d2b60u ? DI_OK : 1u /* DI_NOTATTACHED */, 2);
}
static void m_Initialize(rc_cpu *c) { RC_RETURN(c, DI_OK, 3); }

/* ---- IDirectInputDevice ---- */

/* GetCapabilities(this, DIDEVCAPS*): size, flags (attached), type, axes, buttons, povs. */
static void d_GetCapabilities(rc_cpu *c)
{
    object *o = self(c);
    uint32_t p = RC_ARG(c, 1);
    uint32_t size = rc_ld32(p);
    memset(rc_ptr(p + 4), 0, size > 4 ? size - 4 : 0);
    rc_st32(p + 4, 0x1 | 0x4); /* DIDC_ATTACHED | DIDC_EMULATED */
    if (o->kind == K_KEYBOARD) {
        rc_st32(p + 8, 0x0103); /* DIDEVTYPE_KEYBOARD, PCENH */
        rc_st32(p + 16, 128);   /* buttons */
    } else {
        rc_st32(p + 8, 0x0102); /* DIDEVTYPE_MOUSE */
        rc_st32(p + 12, 3);
        rc_st32(p + 16, 4);
    }
    RC_RETURN(c, DI_OK, 2);
}

static void d_SetProperty(rc_cpu *c)
{
    object *o = self(c);
    if (RC_ARG(c, 1) == DIPROP_BUFFERSIZE) {
        uint32_t n = rc_ld32(RC_ARG(c, 2) + 16);
        o->buffer = n > EVENTS ? EVENTS : n;
        o->head = o->tail = 0;
        if (rc_trace("dinput")) {
            rc_log("dinput: %s buffers %u events", o->kind == K_MOUSE ? "mouse" : "keyboard", n);
        }
    }
    RC_RETURN(c, DI_OK, 3);
}

static void d_GetProperty(rc_cpu *c)
{
    object *o = self(c);
    if (RC_ARG(c, 1) == DIPROP_BUFFERSIZE) {
        rc_st32(RC_ARG(c, 2) + 16, o->buffer);
        RC_RETURN(c, DI_OK, 3);
    }
    RC_RETURN(c, DIERR_UNSUPPORTED, 3);
}

static void d_Acquire(rc_cpu *c)
{
    object *o = self(c);
    if (o->kind == K_MOUSE && !o->acquired) {
        mouse_dx = mouse_dy = mouse_dz = 0;
    }
    o->acquired = true;
    update_capture();
    RC_RETURN(c, DI_OK, 1);
}

static void d_Unacquire(rc_cpu *c)
{
    object *o = self(c);
    o->acquired = false;
    o->head = o->tail = 0;
    update_capture();
    RC_RETURN(c, DI_OK, 1);
}

/* GetDeviceState(this, size, data) */
static void d_GetDeviceState(rc_cpu *c)
{
    object *o = self(c);
    uint32_t n = RC_ARG(c, 1), p = RC_ARG(c, 2);
    if (!o->acquired) {
        RC_RETURN(c, DIERR_NOTACQUIRED, 3);
    }
    if (o->kind == K_KEYBOARD) {
        memcpy(rc_ptr(p), keyboard, n < 256 ? n : 256);
    } else {
        update_capture(); /* (the display may have come after Acquire) */
        /* DIMOUSESTATE (16 bytes) or DIMOUSESTATE2 (20). */
        if (n < 16) {
            RC_RETURN(c, DIERR_INVALIDPARAM, 3);
        }
        memset(rc_ptr(p), 0, n);
        rc_st32(p, (uint32_t)mouse_dx);
        rc_st32(p + 4, (uint32_t)mouse_dy);
        rc_st32(p + 8, (uint32_t)mouse_dz);
        memcpy(rc_ptr(p + 12), buttons, 4);
        mouse_dx = mouse_dy = mouse_dz = 0;
    }
    RC_RETURN(c, DI_OK, 3);
}

/* GetDeviceData(this, object_size, data, inout_count, flags) */
static void d_GetDeviceData(rc_cpu *c)
{
    object *o = self(c);
    uint32_t osize = RC_ARG(c, 1), out = RC_ARG(c, 2), pn = RC_ARG(c, 3), flags = RC_ARG(c, 4);
    if (!o->acquired) {
        RC_RETURN(c, DIERR_NOTACQUIRED, 5);
    }
    uint32_t want = rc_ld32(pn), got = 0, at = o->head;
    while (got < want && at != o->tail) {
        if (out) {
            const devdata *d = &o->ev[at % EVENTS];
            uint32_t p = out + got * osize;
            rc_st32(p, d->ofs);
            rc_st32(p + 4, d->data);
            rc_st32(p + 8, d->time);
            rc_st32(p + 12, d->seq);
        }
        got++;
        at++;
    }
    if (!(flags & DIGDD_PEEK)) {
        o->head = at;
    }
    rc_st32(pn, got);
    uint32_t r = o->overflow ? DI_BUFFEROVERFLOW : DI_OK;
    o->overflow = false;
    RC_RETURN(c, r, 5);
}

static void d_SetDataFormat(rc_cpu *c)
{
    object *o = self(c);
    o->data_size = rc_ld32(RC_ARG(c, 1) + 12);
    RC_RETURN(c, DI_OK, 2);
}

static void d_ok2(rc_cpu *c) { RC_RETURN(c, DI_OK, 2); }
static void d_ok3(rc_cpu *c) { RC_RETURN(c, DI_OK, 3); }
static void d_Poll(rc_cpu *c) { RC_RETURN(c, DI_OK, 1); }

const rc_import_def rc_dinput_defs[] = {
    {DINPUT, "DirectInputCreateA", k_DirectInputCreateA},
    {DINPUT, "DirectInputCreateEx", k_DirectInputCreateEx},
    {COM, "IDirectInput::QueryInterface", m_QueryInterface},
    {COM, "IDirectInput::AddRef", m_AddRef},
    {COM, "IDirectInput::Release", m_Release},
    {COM, "IDirectInput::CreateDevice", m_CreateDevice},
    {COM, "IDirectInput::EnumDevices", m_EnumDevices},
    {COM, "IDirectInput::GetDeviceStatus", m_GetDeviceStatus},
    {COM, "IDirectInput::Initialize", m_Initialize},
    {COM, "IDirectInput::CreateDeviceEx", m_CreateDeviceEx},
    {COM, "IDirectInputDevice::QueryInterface", m_QueryInterface},
    {COM, "IDirectInputDevice::AddRef", m_AddRef},
    {COM, "IDirectInputDevice::Release", m_Release},
    {COM, "IDirectInputDevice::GetCapabilities", d_GetCapabilities},
    {COM, "IDirectInputDevice::GetProperty", d_GetProperty},
    {COM, "IDirectInputDevice::SetProperty", d_SetProperty},
    {COM, "IDirectInputDevice::Acquire", d_Acquire},
    {COM, "IDirectInputDevice::Unacquire", d_Unacquire},
    {COM, "IDirectInputDevice::GetDeviceState", d_GetDeviceState},
    {COM, "IDirectInputDevice::GetDeviceData", d_GetDeviceData},
    {COM, "IDirectInputDevice::SetDataFormat", d_SetDataFormat},
    {COM, "IDirectInputDevice::SetEventNotification", d_ok2},
    {COM, "IDirectInputDevice::SetCooperativeLevel", d_ok3},
    {COM, "IDirectInputDevice::Poll", d_Poll},
};
const uint32_t rc_dinput_count = sizeof rc_dinput_defs / sizeof rc_dinput_defs[0];
