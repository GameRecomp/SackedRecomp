/* SPDX-License-Identifier: MIT */
/*
 * DirectPlay 8 (dpnet): IDirectPlay8Peer and IDirectPlay8Address for the C2 engine's CDPNet,
 * on the network of net.c. One service provider, TCP/IP.
 *
 * DirectPlay 8 tells the game everything through its message handler (DPN_MSGID_*): players
 * arriving and leaving, data received, hosts found, asynchronous operations completing. The
 * messages wait in a queue and are handed to the handler at the message pump and at the end of
 * the calls that make them (never while the handler runs). Asynchronous calls return
 * DPNSUCCESS_PENDING and a handle; their completion is such a message.
 *
 * Each machine has one player. Its data on the network: u32 size of the peer info data, the
 * peer info data (SetPeerInfo), the connect data (Connect), which the host shows the game in
 * DPN_MSGID_INDICATE_CONNECT.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "dplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "rc/hle_modules.h"
#include "rc/log.h"
#include "rc/process.h"
#include "rc/vm.h"

#define PCOM "DPNET_COM"
#define ACOM "DPNADDR_COM"

#define DPN_OK 0u
#define DPNSUCCESS_PENDING 0x0015800eu
#define DPNSUCCESS_EQUAL 0x00158005u
#define DPNSUCCESS_NOTEQUAL 0x0015800au
#define DPNERR_GENERIC 0x80004005u
#define DPNERR_INVALIDPARAM 0x80070057u
#define DPNERR_BUFFERTOOSMALL 0x80158100u
#define DPNERR_CONNECTIONLOST 0x80158160u
#define DPNERR_DOESNOTEXIST 0x80158180u
#define DPNERR_HOSTTERMINATEDSESSION 0x80158270u
#define DPNERR_INVALIDPLAYER 0x801583b0u
#define DPNERR_NOCONNECTION 0x80158400u
#define DPNERR_USERCANCEL 0x80158580u
#define DPNOP_SYNC 0x80000000u

#define DPNID_ALL 0u
#define DPNSEND_NOCOMPLETE 0x2u
#define DPNSEND_NOLOOPBACK 0x20u
#define DPNPLAYER_LOCAL 0x2u
#define DPNPLAYER_HOST 0x4u

/* DPN_MSGID_* */
#define MSG_APPLICATION_DESC 0xffff0002u
#define MSG_ASYNC_OP_COMPLETE 0xffff0003u
#define MSG_CONNECT_COMPLETE 0xffff0005u
#define MSG_CREATE_PLAYER 0xffff0007u
#define MSG_DESTROY_PLAYER 0xffff0009u
#define MSG_ENUM_HOSTS_RESPONSE 0xffff000bu
#define MSG_INDICATE_CONNECT 0xffff000eu
#define MSG_PEER_INFO 0xffff0010u
#define MSG_RECEIVE 0xffff0011u
#define MSG_SEND_COMPLETE 0xffff0014u
#define MSG_TERMINATE_SESSION 0xffff0016u

/* {EBFE7BA0-628D-11D2-AE0F-006097B01411} */
static const uint8_t SP_TCPIP[16] = {0xa0, 0x7b, 0xfe, 0xeb, 0x8d, 0x62, 0xd2, 0x11, 0xae, 0x0f, 0x00, 0x60, 0x97, 0xb0, 0x14, 0x11};

/* ---- wide strings ---- */

static void wide_to_utf8(uint32_t w, char *out, int cap)
{
    int k = 0;
    for (uint32_t i = 0; w && k < cap - 4; i++) {
        uint32_t ch = rc_ld16(w + 2 * i);
        if (!ch) {
            break;
        }
        if (ch < 0x80) {
            out[k++] = (char)ch;
        } else if (ch < 0x800) {
            out[k++] = (char)(0xc0 | ch >> 6);
            out[k++] = (char)(0x80 | (ch & 0x3f));
        } else {
            out[k++] = (char)(0xe0 | ch >> 12);
            out[k++] = (char)(0x80 | (ch >> 6 & 0x3f));
            out[k++] = (char)(0x80 | (ch & 0x3f));
        }
    }
    out[k] = 0;
}

/* UTF-8 to UTF-16 into the guest at `w` (room for `cap` characters); characters written with
 * the terminator. NULL `w`: just count. */
static uint32_t utf8_to_wide(const char *s, uint32_t w, uint32_t cap)
{
    uint32_t k = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t ch = *p++;
        if (ch >= 0xe0 && p[0] && p[1]) {
            ch = (ch & 0x0f) << 12 | (uint32_t)(p[0] & 0x3f) << 6 | (p[1] & 0x3f);
            p += 2;
        } else if (ch >= 0xc0 && p[0]) {
            ch = (ch & 0x1f) << 6 | (p[0] & 0x3f);
            p++;
        }
        if (w && k + 1 < cap) {
            rc_st16(w + 2 * k, (uint16_t)ch);
        }
        k++;
    }
    if (w && cap) {
        rc_st16(w + 2 * (k < cap ? k : cap - 1), 0);
    }
    return k + 1;
}

static void guid_text(const uint8_t g[16], char *out)
{
    uint32_t d1;
    uint16_t d2, d3;
    memcpy(&d1, g, 4);
    memcpy(&d2, g + 4, 2);
    memcpy(&d3, g + 6, 2);
    sprintf(out, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", d1, d2, d3, g[8], g[9], g[10], g[11], g[12], g[13],
            g[14], g[15]);
}

/* ---- IDirectPlay8Address ---- */

#define ADDRS 512
#define COMPS 8
enum { T_WSTRING = 1, T_DWORD = 2, T_GUID = 3, T_BINARY = 4, T_ASTRING = 5 };

typedef struct comp {
    char name[32];
    uint32_t type, size;
    uint8_t value[128];
} comp;

typedef struct addr {
    uint32_t guest, refs;
    comp comps[COMPS];
    int ncomps;
    uint8_t user[64];
    uint32_t user_size;
} addr;

static addr addrs[ADDRS];
static uint32_t addr_vt;

static const char *const addr_methods[] = {
    "QueryInterface", "AddRef", "Release", "BuildFromURLW", "BuildFromURLA", "Duplicate", "SetEqual", "IsEqual",
    "Clear", "GetURLW", "GetURLA", "GetSP", "GetUserData", "SetSP", "SetUserData", "GetNumComponents",
    "GetComponentByName", "GetComponentByIndex", "AddComponent", "GetDevice", "SetDevice", "BuildFromDPADDRESS",
};

static uint32_t make_vtable(const char *iface, const char *dll, const char *const *names, uint32_t count)
{
    uint32_t vt = rc_heap_alloc(4 * count, false);
    for (uint32_t i = 0; i < count; i++) {
        char buf[80];
        snprintf(buf, sizeof buf, "%s::%s", iface, names[i]);
        rc_st32(vt + 4 * i, rc_import_address(dll, rc_str(rc_proc_str(buf))));
    }
    return vt;
}

static addr *addr_of(uint32_t guest)
{
    uint32_t i = guest ? rc_ld32(guest + 4) : ADDRS;
    return i < ADDRS && addrs[i].guest == guest ? &addrs[i] : NULL;
}

static addr *addr_new(void)
{
    if (!addr_vt) {
        addr_vt = make_vtable("IDirectPlay8Address", ACOM, addr_methods, sizeof addr_methods / sizeof *addr_methods);
    }
    for (uint32_t i = 0; i < ADDRS; i++) {
        if (!addrs[i].guest) {
            memset(&addrs[i], 0, sizeof addrs[i]);
            addrs[i].guest = rc_heap_alloc(8, true);
            addrs[i].refs = 1;
            rc_st32(addrs[i].guest, addr_vt);
            rc_st32(addrs[i].guest + 4, i);
            return &addrs[i];
        }
    }
    rc_log("[dpnet] out of address objects");
    return NULL;
}

static comp *comp_find(addr *a, const char *name)
{
    for (int i = 0; i < a->ncomps; i++) {
        if (!strcmp(a->comps[i].name, name)) {
            return &a->comps[i];
        }
    }
    return NULL;
}

static void comp_set(addr *a, const char *name, uint32_t type, const void *value, uint32_t size)
{
    comp *k = comp_find(a, name);
    if (!k && a->ncomps < COMPS) {
        k = &a->comps[a->ncomps++];
    }
    if (!k || size > sizeof k->value) {
        return;
    }
    snprintf(k->name, sizeof k->name, "%s", name);
    k->type = type;
    k->size = size;
    memcpy(k->value, value, size);
}

/* A string component as UTF-8 (strings are kept as UTF-8 here, whatever they came as). */
static bool comp_text(addr *a, const char *name, char *out, int cap)
{
    comp *k = comp_find(a, name);
    if (!k || (k->type != T_WSTRING && k->type != T_ASTRING)) {
        return false;
    }
    snprintf(out, (size_t)cap, "%.*s", (int)k->size, (const char *)k->value);
    return true;
}

/* An address for a host found or a player: provider, hostname, port. */
static uint32_t addr_for(const char *ip, uint16_t port)
{
    addr *a = addr_new();
    if (!a) {
        return 0;
    }
    comp_set(a, "provider", T_GUID, SP_TCPIP, 16);
    comp_set(a, "hostname", T_WSTRING, ip, (uint32_t)strlen(ip) + 1);
    uint32_t p = port;
    comp_set(a, "port", T_DWORD, &p, 4);
    return a->guest;
}

static void addr_release(uint32_t guest)
{
    addr *a = addr_of(guest);
    if (a && a->refs && --a->refs == 0) {
        rc_heap_free(a->guest);
        a->guest = 0;
    }
}

static void a_QueryInterface(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0));
    if (a) {
        a->refs++;
    }
    rc_st32(RC_ARG(c, 2), RC_ARG(c, 0));
    RC_RETURN(c, DPN_OK, 3);
}
static void a_AddRef(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0));
    RC_RETURN(c, a ? ++a->refs : 0, 1);
}
static void a_Release(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0));
    uint32_t left = a && a->refs ? a->refs - 1 : 0;
    addr_release(RC_ARG(c, 0));
    RC_RETURN(c, left, 1);
}

static void build_from_url(addr *a, const char *url)
{
    a->ncomps = 0;
    const char *p = strstr(url, ":/");
    p = p ? p + 2 : url;
    while (*p) {
        char key[32] = {0}, val[128] = {0};
        int kn = 0, vn = 0;
        while (*p && *p != '=' && *p != ';' && kn < 31) {
            key[kn++] = *p++;
        }
        if (*p == '=') {
            p++;
            while (*p && *p != ';' && vn < 127) {
                if (*p == '%' && p[1] && p[2]) {
                    char hex[3] = {p[1], p[2], 0};
                    val[vn++] = (char)strtol(hex, NULL, 16);
                    p += 3;
                } else {
                    val[vn++] = *p++;
                }
            }
        }
        if (*p == ';') {
            p++;
        }
        if (!kn) {
            continue;
        }
        if (!strcmp(key, "port")) {
            uint32_t v = (uint32_t)strtoul(val, NULL, 10);
            comp_set(a, key, T_DWORD, &v, 4);
        } else if (!strcmp(key, "provider") || !strcmp(key, "device")) {
            comp_set(a, key, T_GUID, SP_TCPIP, 16); /* the only provider there is */
        } else {
            comp_set(a, key, T_WSTRING, val, (uint32_t)strlen(val) + 1);
        }
    }
}

static void a_BuildFromURLW(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0));
    char url[256];
    wide_to_utf8(RC_ARG(c, 1), url, sizeof url);
    if (a) {
        build_from_url(a, url);
    }
    RC_RETURN(c, a ? DPN_OK : DPNERR_INVALIDPARAM, 2);
}

static void a_BuildFromURLA(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0));
    const char *url = rc_str(RC_ARG(c, 1));
    if (a && url) {
        build_from_url(a, url);
    }
    RC_RETURN(c, a ? DPN_OK : DPNERR_INVALIDPARAM, 2);
}

static void a_Duplicate(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0)), *b = a ? addr_new() : NULL;
    if (!b) {
        RC_RETURN(c, DPNERR_GENERIC, 2);
    }
    uint32_t g = b->guest;
    *b = *a;
    b->guest = g;
    b->refs = 1;
    rc_st32(RC_ARG(c, 1), g);
    RC_RETURN(c, DPN_OK, 2);
}

static void a_SetEqual(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0)), *b = addr_of(RC_ARG(c, 1));
    if (a && b) {
        uint32_t g = a->guest, r = a->refs;
        *a = *b;
        a->guest = g;
        a->refs = r;
    }
    RC_RETURN(c, a && b ? DPN_OK : DPNERR_INVALIDPARAM, 2);
}

static void url_of(addr *a, char *out, size_t cap)
{
    size_t k = (size_t)snprintf(out, cap, "x-directplay:/");
    for (int i = 0; i < a->ncomps && k < cap; i++) {
        comp *m = &a->comps[i];
        char val[160];
        if (m->type == T_DWORD) {
            uint32_t v;
            memcpy(&v, m->value, 4);
            snprintf(val, sizeof val, "%u", v);
        } else if (m->type == T_GUID) {
            char g[40];
            guid_text(m->value, g);
            snprintf(val, sizeof val, "%%7B%s%%7D", g);
        } else if (m->type == T_WSTRING || m->type == T_ASTRING) {
            snprintf(val, sizeof val, "%.*s", (int)m->size, (const char *)m->value);
        } else {
            continue;
        }
        k += (size_t)snprintf(out + k, cap - k, "%s%s=%s", i ? ";" : "", m->name, val);
    }
}

static void a_IsEqual(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0)), *b = addr_of(RC_ARG(c, 1));
    char x[512], y[512];
    if (!a || !b) {
        RC_RETURN(c, DPNERR_INVALIDPARAM, 2);
    }
    url_of(a, x, sizeof x);
    url_of(b, y, sizeof y);
    RC_RETURN(c, strcmp(x, y) ? DPNSUCCESS_NOTEQUAL : DPNSUCCESS_EQUAL, 2);
}

static void a_Clear(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0));
    if (a) {
        a->ncomps = 0;
        a->user_size = 0;
    }
    RC_RETURN(c, DPN_OK, 1);
}

static void a_GetURLW(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0));
    char url[512];
    if (!a) {
        RC_RETURN(c, DPNERR_INVALIDPARAM, 3);
    }
    url_of(a, url, sizeof url);
    uint32_t need = utf8_to_wide(url, 0, 0), cap = rc_ld32(RC_ARG(c, 2));
    rc_st32(RC_ARG(c, 2), need);
    if (!RC_ARG(c, 1) || cap < need) {
        RC_RETURN(c, DPNERR_BUFFERTOOSMALL, 3);
    }
    utf8_to_wide(url, RC_ARG(c, 1), cap);
    RC_RETURN(c, DPN_OK, 3);
}

static void a_GetURLA(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0));
    char url[512];
    if (!a) {
        RC_RETURN(c, DPNERR_INVALIDPARAM, 3);
    }
    url_of(a, url, sizeof url);
    uint32_t need = (uint32_t)strlen(url) + 1, cap = rc_ld32(RC_ARG(c, 2));
    rc_st32(RC_ARG(c, 2), need);
    if (!RC_ARG(c, 1) || cap < need) {
        RC_RETURN(c, DPNERR_BUFFERTOOSMALL, 3);
    }
    memcpy(rc_ptr(RC_ARG(c, 1)), url, need);
    RC_RETURN(c, DPN_OK, 3);
}

static void a_GetSP(rc_cpu *c)
{
    memcpy(rc_ptr(RC_ARG(c, 1)), SP_TCPIP, 16);
    RC_RETURN(c, DPN_OK, 2);
}
static void a_GetDevice(rc_cpu *c)
{
    memcpy(rc_ptr(RC_ARG(c, 1)), SP_TCPIP, 16);
    RC_RETURN(c, DPN_OK, 2);
}

static void a_SetSP(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0));
    if (a) {
        comp_set(a, "provider", T_GUID, SP_TCPIP, 16);
    }
    RC_RETURN(c, DPN_OK, 2);
}
static void a_SetDevice(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0));
    if (a) {
        comp_set(a, "device", T_GUID, SP_TCPIP, 16);
    }
    RC_RETURN(c, DPN_OK, 2);
}

static void a_GetUserData(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0));
    uint32_t cap = rc_ld32(RC_ARG(c, 2));
    if (!a) {
        RC_RETURN(c, DPNERR_INVALIDPARAM, 3);
    }
    rc_st32(RC_ARG(c, 2), a->user_size);
    if (a->user_size && (!RC_ARG(c, 1) || cap < a->user_size)) {
        RC_RETURN(c, DPNERR_BUFFERTOOSMALL, 3);
    }
    if (a->user_size) {
        memcpy(rc_ptr(RC_ARG(c, 1)), a->user, a->user_size);
    }
    RC_RETURN(c, DPN_OK, 3);
}

static void a_SetUserData(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0));
    uint32_t size = RC_ARG(c, 2);
    if (!a || size > sizeof a->user) {
        RC_RETURN(c, DPNERR_INVALIDPARAM, 3);
    }
    a->user_size = size;
    if (size) {
        memcpy(a->user, rc_ptr(RC_ARG(c, 1)), size);
    }
    RC_RETURN(c, DPN_OK, 3);
}

static void a_GetNumComponents(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0));
    rc_st32(RC_ARG(c, 1), a ? (uint32_t)a->ncomps : 0);
    RC_RETURN(c, DPN_OK, 2);
}

/* A component's value as the guest sees it: strings as UTF-16 (or ANSI), the rest as stored. */
static uint32_t put_value(const comp *k, uint32_t buf, uint32_t psize, uint32_t ptype)
{
    char text[130];
    uint32_t need = k->size;
    if (k->type == T_WSTRING) {
        snprintf(text, sizeof text, "%.*s", (int)k->size, (const char *)k->value);
        need = 2 * utf8_to_wide(text, 0, 0);
    }
    uint32_t cap = rc_ld32(psize);
    rc_st32(psize, need);
    if (ptype) {
        rc_st32(ptype, k->type);
    }
    if (!buf || cap < need) {
        return DPNERR_BUFFERTOOSMALL;
    }
    if (k->type == T_WSTRING) {
        utf8_to_wide(text, buf, need / 2);
    } else {
        memcpy(rc_ptr(buf), k->value, k->size);
    }
    return DPN_OK;
}

/* GetComponentByName(this, name, buffer, size*, type*) */
static void a_GetComponentByName(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0));
    char name[32];
    wide_to_utf8(RC_ARG(c, 1), name, sizeof name);
    comp *k = a ? comp_find(a, name) : NULL;
    if (!k) {
        RC_RETURN(c, DPNERR_DOESNOTEXIST, 5);
    }
    RC_RETURN(c, put_value(k, RC_ARG(c, 2), RC_ARG(c, 3), RC_ARG(c, 4)), 5);
}

/* GetComponentByIndex(this, index, name, name length*, buffer, size*, type*) */
static void a_GetComponentByIndex(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0));
    uint32_t i = RC_ARG(c, 1);
    if (!a || i >= (uint32_t)a->ncomps) {
        RC_RETURN(c, DPNERR_DOESNOTEXIST, 7);
    }
    comp *k = &a->comps[i];
    uint32_t nl = utf8_to_wide(k->name, 0, 0), ncap = rc_ld32(RC_ARG(c, 3));
    rc_st32(RC_ARG(c, 3), nl);
    uint32_t hr = put_value(k, RC_ARG(c, 4), RC_ARG(c, 5), RC_ARG(c, 6));
    if (!RC_ARG(c, 2) || ncap < nl) {
        RC_RETURN(c, DPNERR_BUFFERTOOSMALL, 7);
    }
    utf8_to_wide(k->name, RC_ARG(c, 2), ncap);
    RC_RETURN(c, hr, 7);
}

/* AddComponent(this, name, data, size, type) */
static void a_AddComponent(rc_cpu *c)
{
    addr *a = addr_of(RC_ARG(c, 0));
    char name[32];
    wide_to_utf8(RC_ARG(c, 1), name, sizeof name);
    uint32_t data = RC_ARG(c, 2), size = RC_ARG(c, 3), type = RC_ARG(c, 4);
    if (!a) {
        RC_RETURN(c, DPNERR_INVALIDPARAM, 5);
    }
    if (type == T_WSTRING) {
        char text[128];
        wide_to_utf8(data, text, sizeof text);
        comp_set(a, name, T_WSTRING, text, (uint32_t)strlen(text) + 1);
    } else if (type == T_ASTRING) {
        char text[128];
        snprintf(text, sizeof text, "%.*s", (int)size, (const char *)rc_ptr(data));
        comp_set(a, name, T_WSTRING, text, (uint32_t)strlen(text) + 1);
    } else {
        comp_set(a, name, type, rc_ptr(data), size);
    }
    RC_RETURN(c, DPN_OK, 5);
}

static void a_BuildFromDPADDRESS(rc_cpu *c) { RC_RETURN(c, DPNERR_INVALIDPARAM, 3); }

static uint32_t create_address(rc_cpu *c, uint32_t iid)
{
    (void)c;
    (void)iid;
    addr *a = addr_new();
    return a ? a->guest : 0;
}

/* ---- the message queue ---- */

typedef struct dmsg {
    uint32_t type;
    uint32_t a, b, hr;        /* player id / handle, context, result */
    uint8_t *data;            /* received data, a session (ENUM_HOSTS_RESPONSE) */
    uint32_t size;
    struct dmsg *next;
} dmsg;

static struct {
    uint32_t obj, refs;
    uint32_t handler, user;   /* Initialize */
    dmsg *queue, *tail;
    bool delivering;
    uint32_t next_handle;
    /* SetPeerInfo before the session, and the local player */
    char name[NET_NAME];
    uint8_t *info;
    uint32_t info_size;
    uint32_t local;
    /* EnumHosts in progress */
    uint32_t enum_handle, enum_ctx, enum_retry;
    uint64_t enum_end, enum_next;
    uint8_t enum_app[16];
    uint8_t reported[16][16];
    int nreported;
    uint32_t n_sent, n_received;
} d;

static void post(uint32_t type, uint32_t a, uint32_t b, uint32_t hr, const void *data, uint32_t size)
{
    dmsg *m = calloc(1, sizeof *m);
    if (!m) {
        return;
    }
    m->type = type;
    m->a = a;
    m->b = b;
    m->hr = hr;
    if (size && (m->data = malloc(size)) != NULL) {
        memcpy(m->data, data, size);
        m->size = size;
    }
    if (d.tail) {
        d.tail->next = m;
    } else {
        d.queue = m;
    }
    d.tail = m;
}

static uint32_t new_handle(void) { return ++d.next_handle | 0x4000u; }

/* DPN_APPLICATION_DESC (72 bytes) with the name and application data after it; its size. */
static uint32_t desc_size(const net_session *s) { return 72 + 2 * utf8_to_wide(s->name, 0, 0) + s->app_data_size; }

static void put_app_desc(uint32_t p, const net_session *s)
{
    memset(rc_ptr(p), 0, 72);
    rc_st32(p, 72);
    rc_st32(p + 4, s->flags);
    memcpy(rc_ptr(p + 8), s->instance, 16);
    memcpy(rc_ptr(p + 24), s->app, 16);
    rc_st32(p + 40, s->max_players);
    rc_st32(p + 44, s->cur_players);
    uint32_t name = p + 72, nl = utf8_to_wide(s->name, 0, 0);
    utf8_to_wide(s->name, name, nl);
    rc_st32(p + 48, name);
    if (s->app_data_size) {
        memcpy(rc_ptr(name + 2 * nl), s->app_data, s->app_data_size);
        rc_st32(p + 64, name + 2 * nl);
        rc_st32(p + 68, s->app_data_size);
    }
}

static void get_app_desc(uint32_t p, net_session *s)
{
    memset(s, 0, sizeof *s);
    s->flags = rc_ld32(p + 4);
    memcpy(s->instance, rc_ptr(p + 8), 16);
    memcpy(s->app, rc_ptr(p + 24), 16);
    s->max_players = rc_ld32(p + 40);
    wide_to_utf8(rc_ld32(p + 48), s->name, NET_NAME);
    uint32_t size = rc_ld32(p + 68);
    if (rc_ld32(p + 64) && size) {
        if (size > NET_APPDATA) {
            rc_log("[dpnet] application data of %u bytes cut to %u", size, NET_APPDATA);
            size = NET_APPDATA;
        }
        memcpy(s->app_data, rc_ptr(rc_ld32(p + 64)), size);
        s->app_data_size = size;
    }
}

/* Hand one message to the game's handler; what it answers. */
static uint32_t deliver_one(rc_cpu *c, dmsg *m)
{
    uint32_t msg = rc_heap_alloc(64 + m->size + 1024, true), hr = DPN_OK, args[3];
    net_player *p = NULL;
    uint32_t buf = 0;
    switch (m->type) {
    case MSG_CREATE_PLAYER: /* size, id, context (the game may set it) */
        p = net_player_by_id(m->a);
        rc_st32(msg, 12);
        rc_st32(msg + 4, m->a);
        rc_st32(msg + 8, p ? p->context : m->b);
        break;
    case MSG_DESTROY_PLAYER: /* size, id, context, reason */
        rc_st32(msg, 16);
        rc_st32(msg + 4, m->a);
        rc_st32(msg + 8, m->b);
        rc_st32(msg + 12, m->hr);
        break;
    case MSG_INDICATE_CONNECT: { /* size, connect data, size, reply, size, reply context, player context, addresses */
        p = net_player_by_id(m->a);
        uint32_t cd = msg + 64;
        if (m->size) {
            memcpy(rc_ptr(cd), m->data, m->size);
        }
        char ip[64];
        net_player_ip(m->a, ip, sizeof ip);
        rc_st32(msg, 36);
        rc_st32(msg + 4, m->size ? cd : 0);
        rc_st32(msg + 8, m->size);
        rc_st32(msg + 28, addr_for(ip, 0));
        rc_st32(msg + 32, addr_for("0.0.0.0", 0));
        break;
    }
    case MSG_RECEIVE: /* size, sender, its context, data, size, buffer handle, flags */
        p = net_player_by_id(m->a);
        buf = rc_heap_alloc(m->size ? m->size : 1, false);
        if (m->size) {
            memcpy(rc_ptr(buf), m->data, m->size);
        }
        rc_st32(msg, 24);
        rc_st32(msg + 4, m->a);
        rc_st32(msg + 8, p ? p->context : 0);
        rc_st32(msg + 12, buf);
        rc_st32(msg + 16, m->size);
        rc_st32(msg + 20, buf);
        break;
    case MSG_PEER_INFO: /* size, id, context */
        p = net_player_by_id(m->a);
        rc_st32(msg, 12);
        rc_st32(msg + 4, m->a);
        rc_st32(msg + 8, p ? p->context : 0);
        break;
    case MSG_ASYNC_OP_COMPLETE:
    case MSG_SEND_COMPLETE: /* size, handle, context, result (send: + time) */
        rc_st32(msg, m->type == MSG_SEND_COMPLETE ? 20 : 16);
        rc_st32(msg + 4, m->a);
        rc_st32(msg + 8, m->b);
        rc_st32(msg + 12, m->hr);
        break;
    case MSG_CONNECT_COMPLETE: /* size, handle, context, result, reply data, size */
        rc_st32(msg, 24);
        rc_st32(msg + 4, m->a);
        rc_st32(msg + 8, m->b);
        rc_st32(msg + 12, m->hr);
        break;
    case MSG_ENUM_HOSTS_RESPONSE: { /* size, sender, device, application desc, response, size, context, latency */
        net_session s;
        memcpy(&s, m->data, sizeof s);
        char ip[64] = "127.0.0.1";
        uint16_t port = 0;
        net_session_host(s.instance, ip, sizeof ip, &port);
        uint32_t ad = msg + 64;
        put_app_desc(ad, &s);
        uint32_t sender = addr_for(ip, port);
        addr *sa = addr_of(sender);
        if (sa) {
            comp_set(sa, "rc-instance", T_BINARY, s.instance, 16);
        }
        rc_st32(msg, 32);
        rc_st32(msg + 4, sender);
        rc_st32(msg + 8, addr_for("0.0.0.0", 0));
        rc_st32(msg + 12, ad);
        rc_st32(msg + 24, m->b);
        rc_st32(msg + 28, 10);
        break;
    }
    case MSG_APPLICATION_DESC:
        rc_st32(msg, 4);
        break;
    case MSG_TERMINATE_SESSION: /* size, result, data, size */
        rc_st32(msg, 16);
        rc_st32(msg + 4, m->hr);
        break;
    default:
        break;
    }
    args[0] = d.user;
    args[1] = m->type;
    args[2] = msg;
    hr = rc_call_guest(c, d.handler, 3, args);
    if (rc_trace("dplay")) {
        char hex[3 * 32 + 1] = "";
        for (uint32_t i = 0; i < m->size && i < 32 && m->type != MSG_ENUM_HOSTS_RESPONSE; i++) {
            snprintf(hex + 3 * i, 4, " %02x", m->data[i]);
        }
        rc_log("[dpnet] message 0x%08x (%#x, %u bytes%s) -> 0x%08x", m->type, m->a, m->size, hex, hr);
    }
    /* What the handler gave back. */
    if (m->type == MSG_CREATE_PLAYER && p) {
        p->context = rc_ld32(msg + 8);
    } else if (m->type == MSG_INDICATE_CONNECT && p) {
        p->context = rc_ld32(msg + 24);
        if (hr != DPN_OK) {
            rc_log("[dpnet] the game turned down player %#x (0x%08x); it stays in the session", m->a, hr);
        }
        if (rc_trace("dplay")) {
            rc_log("[dpnet] connect reply: %#x, %u bytes", rc_ld32(msg + 12), rc_ld32(msg + 16));
        }
    } else if (m->type == MSG_RECEIVE && hr != DPNSUCCESS_PENDING) {
        rc_heap_free(buf); /* else the game returns it with ReturnBuffer */
    }
    if (m->type == MSG_ENUM_HOSTS_RESPONSE || m->type == MSG_INDICATE_CONNECT) {
        addr_release(rc_ld32(msg + 4));
        addr_release(rc_ld32(msg + 8));
        if (m->type == MSG_INDICATE_CONNECT) {
            addr_release(rc_ld32(msg + 28));
            addr_release(rc_ld32(msg + 32));
        }
    }
    rc_heap_free(msg);
    return hr;
}

static void deliver(rc_cpu *c)
{
    if (d.delivering || !d.handler) {
        return;
    }
    d.delivering = true;
    while (d.queue) {
        dmsg *m = d.queue;
        d.queue = m->next;
        if (!d.queue) {
            d.tail = NULL;
        }
        deliver_one(c, m);
        free(m->data);
        free(m);
    }
    d.delivering = false;
}

/* ---- events from the network ---- */

/* The peer info data of a player (its network data: u32 size, info, connect data). */
static const uint8_t *peer_data(const net_player *p, uint32_t *size, const uint8_t **connect, uint32_t *csize)
{
    uint32_t k = 0;
    if (p->data_size >= 4) {
        memcpy(&k, p->data, 4);
    }
    if (k > p->data_size - 4 || p->data_size < 4) {
        k = 0;
    }
    *size = k;
    if (connect) {
        *connect = p->data_size >= 4 ? p->data + 4 + k : NULL;
        *csize = p->data_size >= 4 ? p->data_size - 4 - k : 0;
    }
    return p->data_size >= 4 ? p->data + 4 : NULL;
}

static void ev_player_new(net_player *p, bool existing)
{
    if (net_is_host() && !existing) {
        const uint8_t *cd;
        uint32_t ps, cs;
        peer_data(p, &ps, &cd, &cs);
        post(MSG_INDICATE_CONNECT, p->id, 0, 0, cd, cs);
    }
    post(MSG_CREATE_PLAYER, p->id, 0, 0, NULL, 0);
}

static void ev_player_gone(net_player *p)
{
    if (p->local) {
        post(MSG_TERMINATE_SESSION, 0, 0, DPNERR_HOSTTERMINATEDSESSION, NULL, 0);
    }
    post(MSG_DESTROY_PLAYER, p->id, p->context, p->local ? 4 /* HOSTDESTROYEDPLAYER */ : 2 /* CONNECTIONLOST */, NULL, 0);
}

static void ev_player_data(net_player *p) { post(MSG_PEER_INFO, p->id, 0, 0, NULL, 0); }

static void ev_receive(uint32_t from, uint32_t to, const uint8_t *data, uint32_t size)
{
    (void)to;
    d.n_received++;
    post(MSG_RECEIVE, from, 0, 0, data, size);
}

static void ev_session_lost(void)
{
    post(MSG_TERMINATE_SESSION, 0, 0, DPNERR_CONNECTIONLOST, NULL, 0);
    for (int i = 0; i < NET_PLAYERS; i++) {
        net_player *p = net_player_at(i);
        if (p) {
            post(MSG_DESTROY_PLAYER, p->id, p->context, 3 /* SESSIONTERMINATED */, NULL, 0);
        }
    }
}

static void ev_session_changed(const net_session *s)
{
    (void)s;
    post(MSG_APPLICATION_DESC, 0, 0, 0, NULL, 0);
}

static const net_events events = {ev_player_new, ev_player_gone, ev_player_data, ev_receive, ev_session_lost,
                                  ev_session_changed};

/* Enumeration: ask again every retry interval, report what answers, end at the time asked for. */
static void enum_step(void)
{
    if (!d.enum_handle) {
        return;
    }
    uint64_t now = net_ms();
    net_enum_poll();
    const net_session *list;
    int count = net_sessions(&list);
    for (int i = 0; i < count; i++) {
        bool seen = false;
        for (int k = 0; k < d.nreported && !seen; k++) {
            seen = memcmp(d.reported[k], list[i].instance, 16) == 0;
        }
        static const uint8_t zero[16] = {0};
        if (seen || (memcmp(d.enum_app, zero, 16) && memcmp(d.enum_app, list[i].app, 16))) {
            continue;
        }
        if (d.nreported < 16) {
            memcpy(d.reported[d.nreported++], list[i].instance, 16);
        }
        post(MSG_ENUM_HOSTS_RESPONSE, 0, d.enum_ctx, 0, &list[i], sizeof list[i]);
    }
    if (d.enum_end && now >= d.enum_end) {
        post(MSG_ASYNC_OP_COMPLETE, d.enum_handle, d.enum_ctx, DPN_OK, NULL, 0);
        d.enum_handle = 0;
        net_enum_stop();
    } else if (now >= d.enum_next) {
        d.nreported = 0; /* answers of the new round are reported again */
        net_enum_start(NULL, true);
        d.enum_next = now + d.enum_retry;
    }
}

/* ---- IDirectPlay8Peer ---- */

static const char *const peer_methods[] = {
    "QueryInterface", "AddRef", "Release", "Initialize", "EnumServiceProviders", "CancelAsyncOperation",
    "Connect", "SendTo", "GetSendQueueInfo", "Host", "GetApplicationDesc", "SetApplicationDesc",
    "CreateGroup", "DestroyGroup", "AddPlayerToGroup", "RemovePlayerFromGroup", "SetGroupInfo",
    "GetGroupInfo", "EnumPlayersAndGroups", "EnumGroupMembers", "SetPeerInfo", "GetPeerInfo",
    "GetPeerAddress", "GetLocalHostAddresses", "Close", "EnumHosts", "DestroyPeer", "ReturnBuffer",
    "GetPlayerContext", "GetGroupContext", "GetCaps", "SetCaps", "SetSPCaps", "GetSPCaps",
    "GetConnectionInfo", "RegisterLobby", "TerminateSession",
};

static uint32_t create_peer(rc_cpu *c, uint32_t iid)
{
    (void)c;
    (void)iid;
    if (!d.obj) {
        d.obj = rc_heap_alloc(8, true);
        rc_st32(d.obj, make_vtable("IDirectPlay8Peer", PCOM, peer_methods, sizeof peer_methods / sizeof *peer_methods));
    }
    d.refs++;
    return d.obj;
}

static void p_QueryInterface(rc_cpu *c)
{
    d.refs++;
    rc_st32(RC_ARG(c, 2), RC_ARG(c, 0));
    RC_RETURN(c, DPN_OK, 3);
}
static void p_AddRef(rc_cpu *c) { RC_RETURN(c, ++d.refs, 1); }
static void p_Release(rc_cpu *c) { RC_RETURN(c, d.refs ? --d.refs : 0, 1); }

/* Initialize(this, context, handler, flags) */
static void p_Initialize(rc_cpu *c)
{
    d.user = RC_ARG(c, 1);
    d.handler = RC_ARG(c, 2);
    net_set_events(&events);
    RC_RETURN(c, DPN_OK, 4);
}

/* EnumServiceProviders(this, sp, app, buffer, size*, count*, flags): TCP/IP. DPN_SERVICE_PROVIDER_INFO
 * is flags, guid, name*, reserved*, reserved. */
static void p_EnumServiceProviders(rc_cpu *c)
{
    static const char name[] = "DirectPlay8 TCP/IP Service Provider";
    uint32_t buf = RC_ARG(c, 3), psize = RC_ARG(c, 4), pcount = RC_ARG(c, 5);
    uint32_t nl = utf8_to_wide(name, 0, 0), need = 32 + 2 * nl, cap = rc_ld32(psize);
    rc_st32(psize, need);
    rc_st32(pcount, 1);
    if (!buf || cap < need) {
        RC_RETURN(c, DPNERR_BUFFERTOOSMALL, 7);
    }
    memset(rc_ptr(buf), 0, 32);
    memcpy(rc_ptr(buf + 4), SP_TCPIP, 16);
    utf8_to_wide(name, buf + 32, nl);
    rc_st32(buf + 20, buf + 32);
    RC_RETURN(c, DPN_OK, 7);
}

/* CancelAsyncOperation(this, handle, flags): enumerations end (0: everything). */
static void p_CancelAsyncOperation(rc_cpu *c)
{
    uint32_t h = RC_ARG(c, 1);
    if (d.enum_handle && (h == 0 || h == d.enum_handle)) {
        post(MSG_ASYNC_OP_COMPLETE, d.enum_handle, d.enum_ctx, DPNERR_USERCANCEL, NULL, 0);
        d.enum_handle = 0;
        net_enum_stop();
    }
    RC_RETURN(c, DPN_OK, 3);
}

/* The network data of the local player: peer info and connect data. */
static uint8_t *local_data(const void *connect, uint32_t csize, uint32_t *size)
{
    uint8_t *b = malloc(4 + d.info_size + csize);
    if (!b) {
        *size = 0;
        return NULL;
    }
    memcpy(b, &d.info_size, 4);
    if (d.info_size) {
        memcpy(b + 4, d.info, d.info_size);
    }
    if (csize) {
        memcpy(b + 4 + d.info_size, connect, csize);
    }
    *size = 4 + d.info_size + csize;
    return b;
}

/* Host(this, app desc, devices, count, security, credentials, player context, flags) */
static void p_Host(rc_cpu *c)
{
    net_session s;
    get_app_desc(RC_ARG(c, 1), &s);
    net_set_events(&events);
    if (!net_host(&s)) {
        RC_RETURN(c, DPNERR_GENERIC, 8);
    }
    uint32_t size;
    uint8_t *data = local_data(NULL, 0, &size);
    d.local = net_create_player(d.name, d.name, data, size, DPNPLAYER_HOST, RC_ARG(c, 6));
    free(data);
    post(MSG_CREATE_PLAYER, d.local, RC_ARG(c, 6), 0, NULL, 0);
    deliver(c);
    if (rc_trace("dplay")) {
        rc_log("[dpnet] hosting \"%s\", player %#x", s.name, d.local);
    }
    RC_RETURN(c, DPN_OK, 8);
}

/* Connect(this, app desc, host address, device, security, credentials, connect data, size,
 * player context, async context, handle*, flags) */
static void p_Connect(rc_cpu *c)
{
    uint32_t ad = RC_ARG(c, 1), host = RC_ARG(c, 2), cdata = RC_ARG(c, 6), csize = RC_ARG(c, 7);
    uint32_t pctx = RC_ARG(c, 8), actx = RC_ARG(c, 9), phandle = RC_ARG(c, 10), flags = RC_ARG(c, 11);
    uint8_t instance[16] = {0};
    static const uint8_t zero[16] = {0};
    memcpy(instance, rc_ptr(ad + 8), 16);
    addr *ha = addr_of(host);
    comp *hidden = ha ? comp_find(ha, "rc-instance") : NULL;
    if (!memcmp(instance, zero, 16) && hidden) {
        memcpy(instance, hidden->value, 16);
    }
    if (!memcmp(instance, zero, 16)) {
        /* Only a host name: ask it, take its session of this application. */
        char ip[128] = "";
        if (ha) {
            comp_text(ha, "hostname", ip, sizeof ip);
        }
        net_enum_start(ip, true);
        for (uint64_t until = net_ms() + 1500; net_ms() < until;) {
            net_sleep(10);
            net_enum_poll();
        }
        net_enum_stop();
        const net_session *list;
        int count = net_sessions(&list);
        for (int i = 0; i < count; i++) {
            if (!memcmp(list[i].app, rc_ptr(ad + 24), 16)) {
                memcpy(instance, list[i].instance, 16);
                break;
            }
        }
    }
    net_set_events(&events);
    uint32_t hr = DPN_OK;
    if (!net_join(instance)) {
        hr = DPNERR_NOCONNECTION;
    } else {
        uint32_t size;
        uint8_t *data = local_data(csize ? rc_ptr(cdata) : NULL, csize, &size);
        d.local = net_create_player(d.name, d.name, data, size, 0, pctx);
        free(data);
        if (!d.local) {
            net_close();
            hr = DPNERR_NOCONNECTION;
        } else {
            post(MSG_CREATE_PLAYER, d.local, pctx, 0, NULL, 0);
        }
    }
    if (rc_trace("dplay")) {
        rc_log("[dpnet] connect: 0x%08x, player %#x", hr, d.local);
    }
    uint32_t h = new_handle();
    post(MSG_CONNECT_COMPLETE, h, actx, hr, NULL, 0);
    if (flags & DPNOP_SYNC) {
        deliver(c);
        RC_RETURN(c, hr, 12);
    }
    if (phandle) {
        rc_st32(phandle, h);
    }
    RC_RETURN(c, DPNSUCCESS_PENDING, 12);
}

/* SendTo(this, id, buffers, count, timeout, async context, handle*, flags) */
static void p_SendTo(rc_cpu *c)
{
    uint32_t to = RC_ARG(c, 1), bufs = RC_ARG(c, 2), count = RC_ARG(c, 3), actx = RC_ARG(c, 5);
    uint32_t phandle = RC_ARG(c, 6), flags = RC_ARG(c, 7), total = 0;
    for (uint32_t i = 0; i < count; i++) {
        total += rc_ld32(bufs + 8 * i);
    }
    uint8_t *all = malloc(total ? total : 1);
    if (!all) {
        RC_RETURN(c, DPNERR_GENERIC, 8);
    }
    for (uint32_t i = 0, at = 0; i < count; i++) {
        uint32_t len = rc_ld32(bufs + 8 * i);
        memcpy(all + at, rc_ptr(rc_ld32(bufs + 8 * i + 4)), len);
        at += len;
    }
    if (++d.n_sent % 200 == 0 && rc_trace("dplay")) {
        rc_log("[dpnet] %u messages sent, %u received", d.n_sent, d.n_received);
    }
    if (rc_trace("dplay")) {
        char hex[3 * 32 + 1] = "";
        for (uint32_t i = 0; i < total && i < 32; i++) {
            snprintf(hex + 3 * i, 4, " %02x", all[i]);
        }
        rc_log("[dpnet] send to %#x, %u bytes, flags 0x%x:%s", to, total, flags, hex);
    }
    net_send(d.local, to, all, total);
    if (to == DPNID_ALL && !(flags & DPNSEND_NOLOOPBACK)) {
        post(MSG_RECEIVE, d.local, 0, 0, all, total); /* everyone includes the sender */
    }
    free(all);
    if (flags & DPNOP_SYNC) {
        RC_RETURN(c, DPN_OK, 8);
    }
    uint32_t h = new_handle();
    if (!(flags & DPNSEND_NOCOMPLETE)) {
        post(MSG_SEND_COMPLETE, h, actx, DPN_OK, NULL, 0);
    }
    if (phandle) {
        rc_st32(phandle, h);
    }
    RC_RETURN(c, DPNSUCCESS_PENDING, 8);
}

/* GetSendQueueInfo(this, id, messages*, bytes*, flags): sends leave at once. */
static void p_GetSendQueueInfo(rc_cpu *c)
{
    if (RC_ARG(c, 2)) {
        rc_st32(RC_ARG(c, 2), 0);
    }
    if (RC_ARG(c, 3)) {
        rc_st32(RC_ARG(c, 3), 0);
    }
    RC_RETURN(c, DPN_OK, 5);
}

/* GetApplicationDesc(this, buffer, size*, flags) */
static void p_GetApplicationDesc(rc_cpu *c)
{
    const net_session *s = net_current();
    uint32_t buf = RC_ARG(c, 1), psize = RC_ARG(c, 2), need = desc_size(s), cap = rc_ld32(psize);
    rc_st32(psize, need);
    if (!buf || cap < need) {
        RC_RETURN(c, DPNERR_BUFFERTOOSMALL, 4);
    }
    put_app_desc(buf, s);
    RC_RETURN(c, DPN_OK, 4);
}

/* SetApplicationDesc(this, desc, flags): the host's; clients are told (APPLICATION_DESC). */
static void p_SetApplicationDesc(rc_cpu *c)
{
    net_session s;
    get_app_desc(RC_ARG(c, 1), &s);
    memcpy(s.app, net_current()->app, 16);
    net_set_session(&s);
    post(MSG_APPLICATION_DESC, 0, 0, 0, NULL, 0);
    RC_RETURN(c, DPN_OK, 3);
}

/* EnumPlayersAndGroups(this, ids, count*, flags): players only (DPNENUM_PLAYERS). */
static void p_EnumPlayersAndGroups(rc_cpu *c)
{
    net_poll();
    uint32_t ids = RC_ARG(c, 1), pcount = RC_ARG(c, 2), want = RC_ARG(c, 3), cap = rc_ld32(pcount);
    uint32_t k = (want & 1) ? (uint32_t)net_player_count() : 0;
    rc_st32(pcount, k);
    if (k > cap || (k && !ids)) {
        RC_RETURN(c, DPNERR_BUFFERTOOSMALL, 4);
    }
    for (int i = 0, j = 0; i < NET_PLAYERS && (want & 1); i++) {
        net_player *p = net_player_at(i);
        if (p) {
            rc_st32(ids + 4 * (uint32_t)j++, p->id);
        }
    }
    RC_RETURN(c, DPN_OK, 4);
}

/* SetPeerInfo(this, DPN_PLAYER_INFO, async context, handle*, flags): size, which (1 name, 2 data),
 * name*, data*, size, player flags. */
static void p_SetPeerInfo(rc_cpu *c)
{
    uint32_t pi = RC_ARG(c, 1), which = rc_ld32(pi + 4), actx = RC_ARG(c, 2), phandle = RC_ARG(c, 3);
    if (which & 1) {
        wide_to_utf8(rc_ld32(pi + 8), d.name, NET_NAME);
    }
    if (which & 2) {
        free(d.info);
        d.info = NULL;
        d.info_size = rc_ld32(pi + 16);
        if (d.info_size && (d.info = malloc(d.info_size)) != NULL) {
            memcpy(d.info, rc_ptr(rc_ld32(pi + 12)), d.info_size);
        } else {
            d.info_size = 0;
        }
    }
    net_player *me = d.local ? net_player_by_id(d.local) : NULL;
    if (me) {
        const uint8_t *cd;
        uint32_t ps, cs, size;
        peer_data(me, &ps, &cd, &cs);
        uint8_t *keep = cs ? malloc(cs) : NULL;
        if (keep) {
            memcpy(keep, cd, cs);
        }
        uint8_t *data = local_data(keep, keep ? cs : 0, &size);
        net_set_player(d.local, d.name, d.name, data, size, false, false);
        free(data);
        free(keep);
        post(MSG_PEER_INFO, d.local, 0, 0, NULL, 0);
    }
    if (RC_ARG(c, 4) & DPNOP_SYNC) {
        RC_RETURN(c, DPN_OK, 5);
    }
    uint32_t h = new_handle();
    post(MSG_ASYNC_OP_COMPLETE, h, actx, DPN_OK, NULL, 0);
    if (phandle) {
        rc_st32(phandle, h);
    }
    RC_RETURN(c, DPNSUCCESS_PENDING, 5);
}

/* GetPeerInfo(this, id, DPN_PLAYER_INFO, size*, flags) */
static void p_GetPeerInfo(rc_cpu *c)
{
    net_player *p = net_player_by_id(RC_ARG(c, 1));
    if (!p) {
        RC_RETURN(c, DPNERR_INVALIDPLAYER, 5);
    }
    uint32_t ps;
    const uint8_t *pd = peer_data(p, &ps, NULL, NULL);
    uint32_t buf = RC_ARG(c, 2), psize = RC_ARG(c, 3), nl = utf8_to_wide(p->short_name, 0, 0);
    uint32_t need = 24 + 2 * nl + ps, cap = rc_ld32(psize);
    rc_st32(psize, need);
    if (!buf || cap < need) {
        RC_RETURN(c, DPNERR_BUFFERTOOSMALL, 5);
    }
    rc_st32(buf, 24);
    rc_st32(buf + 4, 3);
    utf8_to_wide(p->short_name, buf + 24, nl);
    rc_st32(buf + 8, buf + 24);
    if (ps) {
        memcpy(rc_ptr(buf + 24 + 2 * nl), pd, ps);
    }
    rc_st32(buf + 12, ps ? buf + 24 + 2 * nl : 0);
    rc_st32(buf + 16, ps);
    rc_st32(buf + 20, (p->local ? DPNPLAYER_LOCAL : 0) | (p->flags & DPNPLAYER_HOST));
    RC_RETURN(c, DPN_OK, 5);
}

/* GetPeerAddress(this, id, address*, flags) */
static void p_GetPeerAddress(rc_cpu *c)
{
    if (!net_player_by_id(RC_ARG(c, 1))) {
        RC_RETURN(c, DPNERR_INVALIDPLAYER, 4);
    }
    char ip[64];
    net_player_ip(RC_ARG(c, 1), ip, sizeof ip);
    rc_st32(RC_ARG(c, 2), addr_for(ip, 0));
    RC_RETURN(c, DPN_OK, 4);
}

/* GetLocalHostAddresses(this, addresses, count*, flags) */
static void p_GetLocalHostAddresses(rc_cpu *c)
{
    uint32_t arr = RC_ARG(c, 1), pcount = RC_ARG(c, 2), cap = rc_ld32(pcount);
    rc_st32(pcount, 1);
    if (!arr || cap < 1) {
        RC_RETURN(c, DPNERR_BUFFERTOOSMALL, 4);
    }
    rc_st32(arr, addr_for("127.0.0.1", 0));
    RC_RETURN(c, DPN_OK, 4);
}

/* Close(this, flags): every player is destroyed for the game, then the session ends. */
static void p_Close(rc_cpu *c)
{
    if (d.enum_handle) {
        post(MSG_ASYNC_OP_COMPLETE, d.enum_handle, d.enum_ctx, DPNERR_USERCANCEL, NULL, 0);
        d.enum_handle = 0;
        net_enum_stop();
    }
    for (int i = 0; i < NET_PLAYERS && net_is_open(); i++) {
        net_player *p = net_player_at(i);
        if (p) {
            post(MSG_DESTROY_PLAYER, p->id, p->context, 1 /* NORMAL */, NULL, 0);
        }
    }
    deliver(c);
    net_close();
    d.local = 0;
    d.handler = 0;
    RC_RETURN(c, DPN_OK, 2);
}

/* EnumHosts(this, app desc, host address, device, user data, size, count, retry interval,
 * timeout, context, handle*, flags) */
static void p_EnumHosts(rc_cpu *c)
{
    uint32_t ad = RC_ARG(c, 1), host = RC_ARG(c, 2), count = RC_ARG(c, 6), retry = RC_ARG(c, 7);
    uint32_t timeout = RC_ARG(c, 8), ctx = RC_ARG(c, 9), phandle = RC_ARG(c, 10), flags = RC_ARG(c, 11);
    char ip[128] = "";
    addr *ha = addr_of(host);
    if (ha) {
        comp_text(ha, "hostname", ip, sizeof ip);
    }
    if (d.enum_handle) {
        post(MSG_ASYNC_OP_COMPLETE, d.enum_handle, d.enum_ctx, DPNERR_USERCANCEL, NULL, 0);
    }
    memcpy(d.enum_app, ad ? rc_ptr(ad + 24) : (const uint8_t *)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16);
    d.enum_retry = retry ? retry : 1500;
    d.enum_ctx = ctx;
    d.nreported = 0;
    uint64_t now = net_ms();
    d.enum_end = count == 0xffffffffu ? 0 : now + (uint64_t)(count ? count : 5) * d.enum_retry + (timeout ? timeout : 1500);
    d.enum_next = now + d.enum_retry;
    net_enum_start(ip, true);
    if (rc_trace("dplay")) {
        rc_log("[dpnet] EnumHosts at \"%s\", count %u, retry %u, timeout %u, flags 0x%x", ip, count, retry, timeout,
               flags);
    }
    if (flags & DPNOP_SYNC) {
        d.enum_handle = new_handle();
        while (d.enum_handle) {
            net_sleep(10);
            enum_step();
        }
        deliver(c);
        RC_RETURN(c, DPN_OK, 12);
    }
    d.enum_handle = new_handle();
    if (phandle) {
        rc_st32(phandle, d.enum_handle);
    }
    RC_RETURN(c, DPNSUCCESS_PENDING, 12);
}

/* DestroyPeer(this, id, data, size, flags): the host removes a player. */
static void p_DestroyPeer(rc_cpu *c)
{
    net_player *p = net_player_by_id(RC_ARG(c, 1));
    if (!p || !net_is_host()) {
        RC_RETURN(c, DPNERR_INVALIDPLAYER, 5);
    }
    post(MSG_DESTROY_PLAYER, p->id, p->context, 4 /* HOSTDESTROYEDPLAYER */, NULL, 0);
    net_destroy_player(RC_ARG(c, 1));
    RC_RETURN(c, DPN_OK, 5);
}

/* ReturnBuffer(this, handle, flags): a received buffer the handler kept. */
static void p_ReturnBuffer(rc_cpu *c)
{
    if (RC_ARG(c, 1)) {
        rc_heap_free(RC_ARG(c, 1));
    }
    RC_RETURN(c, DPN_OK, 3);
}

/* GetPlayerContext(this, id, context*, flags) */
static void p_GetPlayerContext(rc_cpu *c)
{
    net_player *p = net_player_by_id(RC_ARG(c, 1));
    if (!p) {
        RC_RETURN(c, DPNERR_INVALIDPLAYER, 4);
    }
    rc_st32(RC_ARG(c, 2), p->context);
    RC_RETURN(c, DPN_OK, 4);
}

/* GetCaps(this, DPN_CAPS, flags): size, flags, connect timeout, retries, keep-alive. */
static void p_GetCaps(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 1);
    rc_st32(p + 4, 0);
    rc_st32(p + 8, 200);
    rc_st32(p + 12, 14);
    rc_st32(p + 16, 60000);
    RC_RETURN(c, DPN_OK, 3);
}

/* GetSPCaps(this, sp, DPN_SP_CAPS, flags): size, flags, threads, enum count, retry, timeout,
 * max enum payload, buffers per thread, system buffer size. */
static void p_GetSPCaps(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 2);
    rc_st32(p + 4, 0);
    rc_st32(p + 8, 2);
    rc_st32(p + 12, 5);
    rc_st32(p + 16, 1500);
    rc_st32(p + 20, 1500);
    rc_st32(p + 24, 983);
    rc_st32(p + 28, 1);
    rc_st32(p + 32, 8192);
    RC_RETURN(c, DPN_OK, 4);
}

/* GetConnectionInfo(this, id, DPN_CONNECTION_INFO, flags): latency 20 ms, the rest 0. */
static void p_GetConnectionInfo(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 2), size = rc_ld32(p);
    if (size > 4 && size < 512) {
        memset(rc_ptr(p + 4), 0, size - 4);
    }
    rc_st32(p + 4, 20);
    rc_st32(p + 8, 1000000);
    rc_st32(p + 12, 1000000);
    RC_RETURN(c, DPN_OK, 4);
}

/* TerminateSession(this, data, size, flags): the host ends it for everyone. */
static void p_TerminateSession(rc_cpu *c)
{
    post(MSG_TERMINATE_SESSION, 0, 0, DPNERR_HOSTTERMINATEDSESSION, NULL, 0);
    for (int i = 0; i < NET_PLAYERS; i++) {
        net_player *p = net_player_at(i);
        if (p) {
            post(MSG_DESTROY_PLAYER, p->id, p->context, 3 /* SESSIONTERMINATED */, NULL, 0);
        }
    }
    deliver(c);
    net_close();
    d.local = 0;
    RC_RETURN(c, DPN_OK, 4);
}

static void p_ok3(rc_cpu *c) { RC_RETURN(c, DPN_OK, 3); }
static void p_ok4(rc_cpu *c) { RC_RETURN(c, DPN_OK, 4); }

static const rc_import_def defs[] = {
    {PCOM, "IDirectPlay8Peer::QueryInterface", p_QueryInterface},
    {PCOM, "IDirectPlay8Peer::AddRef", p_AddRef},
    {PCOM, "IDirectPlay8Peer::Release", p_Release},
    {PCOM, "IDirectPlay8Peer::Initialize", p_Initialize},
    {PCOM, "IDirectPlay8Peer::EnumServiceProviders", p_EnumServiceProviders},
    {PCOM, "IDirectPlay8Peer::CancelAsyncOperation", p_CancelAsyncOperation},
    {PCOM, "IDirectPlay8Peer::Connect", p_Connect},
    {PCOM, "IDirectPlay8Peer::SendTo", p_SendTo},
    {PCOM, "IDirectPlay8Peer::GetSendQueueInfo", p_GetSendQueueInfo},
    {PCOM, "IDirectPlay8Peer::Host", p_Host},
    {PCOM, "IDirectPlay8Peer::GetApplicationDesc", p_GetApplicationDesc},
    {PCOM, "IDirectPlay8Peer::SetApplicationDesc", p_SetApplicationDesc},
    {PCOM, "IDirectPlay8Peer::EnumPlayersAndGroups", p_EnumPlayersAndGroups},
    {PCOM, "IDirectPlay8Peer::SetPeerInfo", p_SetPeerInfo},
    {PCOM, "IDirectPlay8Peer::GetPeerInfo", p_GetPeerInfo},
    {PCOM, "IDirectPlay8Peer::GetPeerAddress", p_GetPeerAddress},
    {PCOM, "IDirectPlay8Peer::GetLocalHostAddresses", p_GetLocalHostAddresses},
    {PCOM, "IDirectPlay8Peer::Close", p_Close},
    {PCOM, "IDirectPlay8Peer::EnumHosts", p_EnumHosts},
    {PCOM, "IDirectPlay8Peer::DestroyPeer", p_DestroyPeer},
    {PCOM, "IDirectPlay8Peer::ReturnBuffer", p_ReturnBuffer},
    {PCOM, "IDirectPlay8Peer::GetPlayerContext", p_GetPlayerContext},
    {PCOM, "IDirectPlay8Peer::GetCaps", p_GetCaps},
    {PCOM, "IDirectPlay8Peer::SetCaps", p_ok3},
    {PCOM, "IDirectPlay8Peer::GetSPCaps", p_GetSPCaps},
    {PCOM, "IDirectPlay8Peer::SetSPCaps", p_ok4},
    {PCOM, "IDirectPlay8Peer::GetConnectionInfo", p_GetConnectionInfo},
    {PCOM, "IDirectPlay8Peer::RegisterLobby", p_ok4},
    {PCOM, "IDirectPlay8Peer::TerminateSession", p_TerminateSession},
    {ACOM, "IDirectPlay8Address::QueryInterface", a_QueryInterface},
    {ACOM, "IDirectPlay8Address::AddRef", a_AddRef},
    {ACOM, "IDirectPlay8Address::Release", a_Release},
    {ACOM, "IDirectPlay8Address::BuildFromURLW", a_BuildFromURLW},
    {ACOM, "IDirectPlay8Address::BuildFromURLA", a_BuildFromURLA},
    {ACOM, "IDirectPlay8Address::Duplicate", a_Duplicate},
    {ACOM, "IDirectPlay8Address::SetEqual", a_SetEqual},
    {ACOM, "IDirectPlay8Address::IsEqual", a_IsEqual},
    {ACOM, "IDirectPlay8Address::Clear", a_Clear},
    {ACOM, "IDirectPlay8Address::GetURLW", a_GetURLW},
    {ACOM, "IDirectPlay8Address::GetURLA", a_GetURLA},
    {ACOM, "IDirectPlay8Address::GetSP", a_GetSP},
    {ACOM, "IDirectPlay8Address::GetUserData", a_GetUserData},
    {ACOM, "IDirectPlay8Address::SetSP", a_SetSP},
    {ACOM, "IDirectPlay8Address::SetUserData", a_SetUserData},
    {ACOM, "IDirectPlay8Address::GetNumComponents", a_GetNumComponents},
    {ACOM, "IDirectPlay8Address::GetComponentByName", a_GetComponentByName},
    {ACOM, "IDirectPlay8Address::GetComponentByIndex", a_GetComponentByIndex},
    {ACOM, "IDirectPlay8Address::AddComponent", a_AddComponent},
    {ACOM, "IDirectPlay8Address::GetDevice", a_GetDevice},
    {ACOM, "IDirectPlay8Address::SetDevice", a_SetDevice},
    {ACOM, "IDirectPlay8Address::BuildFromDPADDRESS", a_BuildFromDPADDRESS},
};

/* At every message pump: the network, the enumeration, the messages for the game. */
static void pump(rc_cpu *c)
{
    if (!d.handler) {
        return;
    }
    if (net_is_open()) {
        net_poll();
    }
    enum_step();
    deliver(c);
}

void rc_dpnet_init(void)
{
    rc_add_pump_hook(pump);
    rc_register_imports(defs, sizeof defs / sizeof defs[0]);
    rc_com_register("{286F484D-375E-4458-A272-B138E2F80A6A}", create_peer);    /* CLSID_DirectPlay8Peer */
    rc_com_register("{934A9523-A3CA-4BC5-ADA0-D6D95D979421}", create_address); /* CLSID_DirectPlay8Address */
}
