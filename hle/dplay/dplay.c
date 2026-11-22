/* SPDX-License-Identifier: MIT */
/*
 * DirectPlay 4 (dplayx, the ANSI interfaces) and the lobby's address functions, for Cultures 1's
 * lifted DPlayDLL.dll, on the network of net.c. One service provider is offered, TCP/IP.
 * Messages wait in a queue until the game receives them; arrivals, departures and data changes
 * become DirectPlay's system messages there. Delivery is guaranteed and in order. Groups are
 * accepted but have no members. What the wrapper depends on: RESEARCH, "DirectPlay".
 *
 * COM objects are as in ddraw.c; one object answers for every IDirectPlay version.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "dplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel32/k32.h"
#include "net.h"
#include "rc/hle_modules.h"
#include "rc/log.h"
#include "rc/process.h"
#include "rc/vm.h"

#define COM "DPLAY_COM"
#define LCOM "DPLAYLOBBY_COM"
#define DP_OK 0u
#define DPERR_BUFFERTOOSMALL 0x8877001eu
#define DPERR_INVALIDPLAYER 0x88770096u
#define DPERR_NOMESSAGES 0x887700beu
#define DPERR_NOSESSIONS 0x887700dcu
#define DPERR_CONNECTING 0x8877015eu
#define DPERR_NOTLOBBIED 0x8877042eu
#define DPERR_INVALIDPARAMS 0x80070057u
#define DPERR_GENERIC 0x80004005u
#define DPERR_UNSUPPORTED 0x80004001u

#define DPSYS_CREATEPLAYERORGROUP 3u
#define DPSYS_DESTROYPLAYERORGROUP 5u
#define DPSYS_SESSIONLOST 0x31u
#define DPSYS_SETPLAYERORGROUPDATA 0x102u

static const uint8_t SP_TCPIP[16] = {0xe0, 0x5e, 0xe9, 0x36, 0x77, 0x85, 0xcf, 0x11, 0x96, 0x0c, 0x00, 0x80, 0xc7, 0x53, 0x4e, 0x82};
static const uint8_t AID_SP[16] = {0xc0, 0x16, 0xd9, 0x07, 0xaf, 0xe0, 0xcf, 0x11, 0x9c, 0x4e, 0x00, 0xa0, 0xc9, 0x05, 0x42, 0x5e};
static const uint8_t AID_INET[16] = {0xa0, 0x4d, 0xa5, 0xc4, 0xaf, 0xe0, 0xcf, 0x11, 0x9c, 0x4e, 0x00, 0xa0, 0xc9, 0x05, 0x42, 0x5e};

/* ---- the message queue ---- */

typedef struct msg {
    uint32_t from, to, size;
    uint8_t *data;
    struct msg *next;
} msg;

static msg *queue, *tail;
static uint32_t n_sent, n_received; /* game messages (RC_TRACE=dplay logs them now and then) */
static char typed_host[64];         /* the address the player typed (DPAID_INet) */

static void enqueue(uint32_t from, uint32_t to, const void *data, uint32_t size)
{
    msg *m = calloc(1, sizeof *m);
    if (!m || !(m->data = malloc(size ? size : 1))) {
        free(m);
        return;
    }
    m->from = from;
    m->to = to;
    m->size = size;
    memcpy(m->data, data, size);
    if (tail) {
        tail->next = m;
    } else {
        queue = m;
    }
    tail = m;
    /* Each local player's event (CreatePlayer) is set when a message for it arrives. */
    for (int i = 0; i < NET_PLAYERS; i++) {
        net_player *p = net_player_at(i);
        if (p && p->local && p->context && (to == 0 || to == p->id)) {
            rc_k32_set_event(p->context);
        }
    }
}

static void clear_queue(void)
{
    while (queue) {
        msg *m = queue;
        queue = m->next;
        free(m->data);
        free(m);
    }
    tail = NULL;
}

/* A system message about a player (to all local players). The names and the data follow it;
 * Receive points the message's pointers at them.
 *   CREATE (48 bytes): type, player type, id, current players, data*, size, DPNAME at 24, parent, flags
 *   DESTROY (52 bytes): type, player type, id, local data*, size, remote data*, size, DPNAME at 28, ...
 *   SETDATA (20 bytes): type, player type, id, data*, size */
#define SYS_HEAD 64
static void sys_player(uint32_t type, const net_player *p)
{
    uint32_t size = SYS_HEAD + 2 * NET_NAME + p->data_size;
    uint8_t *m = calloc(1, size);
    if (!m) {
        return;
    }
    uint32_t w[3] = {type, 1 /* DPPLAYERTYPE_PLAYER */, p->id};
    memcpy(m, w, sizeof w);
    memcpy(m + SYS_HEAD, p->short_name, NET_NAME);
    memcpy(m + SYS_HEAD + NET_NAME, p->long_name, NET_NAME);
    if (p->data_size) {
        memcpy(m + SYS_HEAD + 2 * NET_NAME, p->data, p->data_size);
    }
    enqueue(0, 0, m, size);
    free(m);
}

static void ev_player_new(net_player *p, bool existing)
{
    if (!existing) {
        sys_player(DPSYS_CREATEPLAYERORGROUP, p);
    }
}
static void ev_player_gone(net_player *p) { sys_player(DPSYS_DESTROYPLAYERORGROUP, p); }
static void ev_player_data(net_player *p) { sys_player(DPSYS_SETPLAYERORGROUPDATA, p); }
static void ev_receive(uint32_t from, uint32_t to, const uint8_t *data, uint32_t size) { enqueue(from, to, data, size); }
static void ev_session_lost(void)
{
    uint32_t lost = DPSYS_SESSIONLOST;
    enqueue(0, 0, &lost, 4);
}

/* ---- IDirectPlay4A ---- */

static const char *const dp_methods[] = {
    "QueryInterface", "AddRef", "Release", "AddPlayerToGroup", "Close", "CreateGroup", "CreatePlayer",
    "DeletePlayerFromGroup", "DestroyGroup", "DestroyPlayer", "EnumGroupPlayers", "EnumGroups",
    "EnumPlayers", "EnumSessions", "GetCaps", "GetGroupData", "GetGroupName", "GetMessageCount",
    "GetPlayerAddress", "GetPlayerCaps", "GetPlayerData", "GetPlayerName", "GetSessionDesc",
    "Initialize", "Open", "Receive", "Send", "SetGroupData", "SetGroupName", "SetPlayerData",
    "SetPlayerName", "SetSessionDesc", "AddGroupToGroup", "CreateGroupInGroup", "DeleteGroupFromGroup",
    "EnumConnections", "EnumGroupsInGroup", "GetGroupConnectionSettings", "InitializeConnection",
    "SecureOpen", "SendChatMessage", "SetGroupConnectionSettings", "StartSession", "GetGroupFlags",
    "GetGroupParent", "GetPlayerAccount", "GetPlayerFlags", "GetGroupOwner", "SetGroupOwner", "SendEx",
    "GetMessageQueue", "CancelMessage", "CancelPriority",
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

static uint32_t refs;

static uint32_t create(rc_cpu *c, uint32_t iid)
{
    (void)c;
    (void)iid;
    static uint32_t obj;
    if (!obj) {
        obj = rc_heap_alloc(8, true);
        rc_st32(obj, make_vtable("IDirectPlay4A", COM, dp_methods, sizeof dp_methods / sizeof *dp_methods));
    }
    refs++;
    return obj;
}

static void m_QueryInterface(rc_cpu *c)
{
    refs++;
    rc_st32(RC_ARG(c, 2), RC_ARG(c, 0));
    RC_RETURN(c, DP_OK, 3);
}
static void m_AddRef(rc_cpu *c) { RC_RETURN(c, ++refs, 1); }
static void m_Release(rc_cpu *c) { RC_RETURN(c, refs ? --refs : 0, 1); }

/* EnumConnections(this, app guid, callback, context, flags): TCP/IP. */
static void m_EnumConnections(rc_cpu *c)
{
    uint32_t cb = RC_ARG(c, 2), ctx = RC_ARG(c, 3);
    static uint32_t sp_name;
    if (!sp_name) {
        sp_name = rc_proc_str("Internet TCP/IP Connection For DirectPlay");
    }
    uint32_t b = rc_heap_alloc(16 + 36 + 16, true), guid = b, addr = b + 16, name = b + 52;
    memcpy(rc_ptr(guid), SP_TCPIP, 16);
    /* The connection: one DPADDRESS element, DPAID_ServiceProvider = TCP/IP. */
    memcpy(rc_ptr(addr), AID_SP, 16);
    rc_st32(addr + 16, 16);
    memcpy(rc_ptr(addr + 20), SP_TCPIP, 16);
    rc_st32(name, 16);
    rc_st32(name + 8, sp_name);
    rc_st32(name + 12, sp_name);
    uint32_t args[6] = {guid, addr, 36, name, 0, ctx};
    rc_call_guest(c, cb, 6, args);
    rc_heap_free(b);
    RC_RETURN(c, DP_OK, 5);
}

/* InitializeConnection(this, connection, flags): a DPADDRESS, a chain of (guid, size, data). */
static void m_InitializeConnection(rc_cpu *c)
{
    uint32_t a = RC_ARG(c, 1);
    typed_host[0] = 0;
    for (int i = 0; a && i < 16; i++) {
        uint32_t len = rc_ld32(a + 16);
        if (len > 4096) {
            break;
        }
        if (memcmp(rc_ptr(a), AID_INET, 16) == 0 && len) {
            uint32_t k = len < sizeof typed_host ? len : sizeof typed_host - 1;
            memcpy(typed_host, rc_ptr(a + 20), k);
            typed_host[k] = 0;
            break;
        }
        if (memcmp(rc_ptr(a), AID_SP, 16) != 0) {
            break;
        }
        a += 20 + len;
    }
    RC_RETURN(c, DP_OK, 3);
}

/* DPSESSIONDESC2 (80 bytes) and the session name after it (callers leave 64 bytes). */
static void put_desc(uint32_t d, const net_session *s)
{
    memset(rc_ptr(d), 0, 80);
    rc_st32(d, 80);
    rc_st32(d + 4, s->flags);
    memcpy(rc_ptr(d + 8), s->instance, 16);
    memcpy(rc_ptr(d + 24), s->app, 16);
    rc_st32(d + 40, s->max_players);
    rc_st32(d + 44, s->cur_players);
    memcpy(rc_ptr(d + 64), s->app_data, 16); /* dwUser1..4 */
    uint32_t len = (uint32_t)strnlen(s->name, NET_NAME - 1);
    memcpy(rc_ptr(d + 80), s->name, len);
    rc_st8(d + 80 + len, 0);
    rc_st32(d + 48, d + 80);
}

static void get_desc(uint32_t d, net_session *s)
{
    memset(s, 0, sizeof *s);
    s->flags = rc_ld32(d + 4);
    memcpy(s->app, rc_ptr(d + 24), 16);
    s->max_players = rc_ld32(d + 40);
    memcpy(s->app_data, rc_ptr(d + 64), 16);
    s->app_data_size = 16;
    const char *name = rc_str(rc_ld32(d + 48));
    snprintf(s->name, NET_NAME, "%s", name ? name : "Game");
}

/* EnumSessions(this, desc, timeout, callback, context, flags). Synchronous: ask, wait the
 * timeout (at most 3 s), report. DPENUMSESSIONS_ASYNC (0x10): report what is known and keep
 * asking in the background; with DPENUMSESSIONS_RETURNSTATUS (0x80) the first call answers
 * DPERR_CONNECTING while the first answers are on their way, as DirectPlay does. */
static void m_EnumSessions(rc_cpu *c)
{
    uint32_t desc = RC_ARG(c, 1), timeout = RC_ARG(c, 2), cb = RC_ARG(c, 3), ctx = RC_ARG(c, 4), flags = RC_ARG(c, 5);
    if (flags & 0x10) {
        if (!net_enum_running()) {
            net_enum_start(typed_host, true);
            if (flags & 0x80) {
                RC_RETURN(c, DPERR_CONNECTING, 6);
            }
        } else if (net_ms() - net_enum_started() > 1000) {
            net_enum_start(typed_host, false);
        }
        net_enum_poll();
    } else {
        net_enum_start(typed_host, true);
        uint64_t until = net_ms() + (timeout && timeout < 3000 ? timeout : 1000);
        do {
            net_sleep(10);
            net_enum_poll();
        } while (net_ms() < until);
        net_enum_stop();
    }
    const net_session *list;
    int count = net_sessions(&list);
    static const uint8_t zero[16] = {0};
    bool any_app = memcmp(rc_ptr(desc + 24), zero, 16) == 0;
    if (rc_trace("dplay")) {
        rc_log("[dplay] EnumSessions flags 0x%x: %d sessions", flags, count);
    }
    uint32_t d = rc_heap_alloc(80 + NET_NAME, true), t = rc_heap_alloc(4, true);
    for (int i = 0; i < count; i++) {
        if (!any_app && memcmp(list[i].app, rc_ptr(desc + 24), 16) != 0) {
            continue;
        }
        put_desc(d, &list[i]);
        rc_st32(t, timeout);
        uint32_t args[4] = {d, t, 0, ctx};
        if (!rc_call_guest(c, cb, 4, args)) {
            break;
        }
    }
    uint32_t args[4] = {0, t, 1 /* DPESC_TIMEDOUT */, ctx};
    rc_call_guest(c, cb, 4, args);
    rc_heap_free(d);
    rc_heap_free(t);
    RC_RETURN(c, DP_OK, 6);
}

/* Open(this, desc, flags): DPOPEN_CREATE (2) hosts, DPOPEN_JOIN (1) joins a session found. */
static const net_events events = {ev_player_new, ev_player_gone, ev_player_data, ev_receive, ev_session_lost, NULL};

static uint32_t open_session(uint32_t desc, uint32_t flags)
{
    clear_queue();
    net_set_events(&events);
    if (flags & 2) {
        net_session s;
        get_desc(desc, &s);
        return net_host(&s) ? DP_OK : DPERR_GENERIC;
    }
    return net_join(rc_ptr(desc + 8)) ? DP_OK : DPERR_NOSESSIONS;
}

static void m_Open(rc_cpu *c) { RC_RETURN(c, open_session(RC_ARG(c, 1), RC_ARG(c, 2)), 3); }

/* SecureOpen(this, desc, flags, security, credentials): no security here. */
static void m_SecureOpen(rc_cpu *c) { RC_RETURN(c, open_session(RC_ARG(c, 1), RC_ARG(c, 2)), 5); }

static void m_Close(rc_cpu *c)
{
    net_enum_stop();
    net_close();
    clear_queue();
    RC_RETURN(c, DP_OK, 1);
}

static void names_from(uint32_t dpname, char sn[NET_NAME], char ln[NET_NAME])
{
    sn[0] = ln[0] = 0;
    if (dpname) {
        const char *s = rc_str(rc_ld32(dpname + 8)), *l = rc_str(rc_ld32(dpname + 12));
        snprintf(sn, NET_NAME, "%s", s ? s : "");
        snprintf(ln, NET_NAME, "%s", l ? l : "");
    }
}

/* CreatePlayer(this, id*, name, event, data, size, flags) */
static void m_CreatePlayer(rc_cpu *c)
{
    char sn[NET_NAME], ln[NET_NAME];
    names_from(RC_ARG(c, 2), sn, ln);
    uint32_t event = RC_ARG(c, 3), size = RC_ARG(c, 5);
    uint32_t id = net_create_player(sn, ln, size ? rc_ptr(RC_ARG(c, 4)) : NULL, size, RC_ARG(c, 6), event);
    if (!id) {
        RC_RETURN(c, DPERR_GENERIC, 7);
    }
    if (queue && event) {
        rc_k32_set_event(event);
    }
    rc_st32(RC_ARG(c, 1), id);
    RC_RETURN(c, DP_OK, 7);
}

static void m_DestroyPlayer(rc_cpu *c) { RC_RETURN(c, net_destroy_player(RC_ARG(c, 1)) ? DP_OK : DPERR_INVALIDPLAYER, 2); }

/* SetPlayerData(this, id, data, size, flags): DPSET_LOCAL (1) keeps it on this machine. */
static void m_SetPlayerData(rc_cpu *c)
{
    uint32_t size = RC_ARG(c, 3);
    bool ok = net_set_player(RC_ARG(c, 1), NULL, NULL, size ? rc_ptr(RC_ARG(c, 2)) : NULL, size, false,
                             (RC_ARG(c, 4) & 1) != 0);
    RC_RETURN(c, ok ? DP_OK : DPERR_INVALIDPLAYER, 5);
}

/* SetPlayerName(this, id, name, flags) */
static void m_SetPlayerName(rc_cpu *c)
{
    char sn[NET_NAME], ln[NET_NAME];
    names_from(RC_ARG(c, 2), sn, ln);
    bool ok = net_set_player(RC_ARG(c, 1), sn, ln, NULL, 0, true, (RC_ARG(c, 3) & 1) != 0);
    RC_RETURN(c, ok ? DP_OK : DPERR_INVALIDPLAYER, 4);
}

/* GetPlayerData(this, id, data, size*, flags) */
static void m_GetPlayerData(rc_cpu *c)
{
    net_player *p = net_player_by_id(RC_ARG(c, 1));
    if (!p) {
        RC_RETURN(c, DPERR_INVALIDPLAYER, 5);
    }
    uint32_t data = RC_ARG(c, 2), psize = RC_ARG(c, 3), cap = rc_ld32(psize);
    rc_st32(psize, p->data_size);
    if (p->data_size && (!data || cap < p->data_size)) {
        RC_RETURN(c, DPERR_BUFFERTOOSMALL, 5);
    }
    if (p->data_size) {
        memcpy(rc_ptr(data), p->data, p->data_size);
    }
    RC_RETURN(c, DP_OK, 5);
}

static void send_msg(uint32_t from, uint32_t to, uint32_t data, uint32_t size)
{
    if (++n_sent % 500 == 0 && rc_trace("dplay")) {
        rc_log("[dplay] %u messages sent, %u received", n_sent, n_received);
    }
    net_send(from, to, rc_ptr(data), size);
}

/* Send(this, from, to, flags, data, size) */
static void m_Send(rc_cpu *c)
{
    send_msg(RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 4), RC_ARG(c, 5));
    RC_RETURN(c, DP_OK, 6);
}

/* SendEx(this, from, to, flags, data, size, priority, timeout, context, id*) */
static void m_SendEx(rc_cpu *c)
{
    send_msg(RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 4), RC_ARG(c, 5));
    if (RC_ARG(c, 9)) {
        rc_st32(RC_ARG(c, 9), 0);
    }
    RC_RETURN(c, DP_OK, 10);
}

/* Receive(this, from*, to*, flags, data, size*) */
static void m_Receive(rc_cpu *c)
{
    net_poll();
    uint32_t pfrom = RC_ARG(c, 1), pto = RC_ARG(c, 2), flags = RC_ARG(c, 3), data = RC_ARG(c, 4), psize = RC_ARG(c, 5);
    msg *m = queue, *prev = NULL;
    for (; m; prev = m, m = m->next) {
        if ((flags & 2) && pto && m->to && m->to != rc_ld32(pto)) { /* DPRECEIVE_TOPLAYER */
            continue;
        }
        if ((flags & 4) && pfrom && m->from != rc_ld32(pfrom)) { /* DPRECEIVE_FROMPLAYER */
            continue;
        }
        break;
    }
    if (!m) {
        RC_RETURN(c, DPERR_NOMESSAGES, 6);
    }
    uint32_t cap = rc_ld32(psize);
    rc_st32(psize, m->size);
    if (!data || cap < m->size) {
        RC_RETURN(c, DPERR_BUFFERTOOSMALL, 6);
    }
    memcpy(rc_ptr(data), m->data, m->size);
    if (m->from == 0 && m->size >= SYS_HEAD + 2 * NET_NAME) {
        /* Point the system message at the names and the data that follow it (see sys_player). */
        uint32_t type = rc_ld32(data), names = data + SYS_HEAD, pdata = names + 2 * NET_NAME;
        uint32_t dsize = m->size - SYS_HEAD - 2 * NET_NAME;
        if (type == DPSYS_CREATEPLAYERORGROUP) {
            rc_st32(data + 12, (uint32_t)net_player_count());
            rc_st32(data + 16, dsize ? pdata : 0);
            rc_st32(data + 20, dsize);
            rc_st32(data + 24, 16);
            rc_st32(data + 32, names);
            rc_st32(data + 36, names + NET_NAME);
        } else if (type == DPSYS_DESTROYPLAYERORGROUP) {
            rc_st32(data + 20, dsize ? pdata : 0);
            rc_st32(data + 24, dsize);
            rc_st32(data + 28, 16);
            rc_st32(data + 36, names);
            rc_st32(data + 40, names + NET_NAME);
        } else if (type == DPSYS_SETPLAYERORGROUPDATA) {
            rc_st32(data + 12, dsize ? pdata : 0);
            rc_st32(data + 16, dsize);
        }
    }
    n_received += m->from != 0;
    rc_st32(pfrom, m->from);
    if (pto) {
        rc_st32(pto, m->to);
    }
    if (!(flags & 8)) { /* not DPRECEIVE_PEEK */
        if (prev) {
            prev->next = m->next;
        } else {
            queue = m->next;
        }
        if (tail == m) {
            tail = prev;
        }
        free(m->data);
        free(m);
    }
    RC_RETURN(c, DP_OK, 6);
}

/* GetMessageCount(this, id, count*) */
static void m_GetMessageCount(rc_cpu *c)
{
    net_poll();
    uint32_t k = 0;
    for (msg *m = queue; m; m = m->next) {
        k++;
    }
    rc_st32(RC_ARG(c, 2), k);
    RC_RETURN(c, DP_OK, 3);
}

/* GetMessageQueue(this, from, to, flags, messages*, bytes*): DPMESSAGEQUEUE_SEND (1): sends
 * leave at once, that queue is empty. RECEIVE (2): ours. */
static void m_GetMessageQueue(rc_cpu *c)
{
    net_poll();
    uint32_t k = 0, bytes = 0, to = RC_ARG(c, 2);
    for (msg *m = (RC_ARG(c, 3) & 2) ? queue : NULL; m; m = m->next) {
        if (!to || !m->to || m->to == to) {
            k++;
            bytes += m->size;
        }
    }
    if (RC_ARG(c, 4)) {
        rc_st32(RC_ARG(c, 4), k);
    }
    if (RC_ARG(c, 5)) {
        rc_st32(RC_ARG(c, 5), bytes);
    }
    RC_RETURN(c, DP_OK, 6);
}

/* EnumPlayers(this, guid, callback, context, flags). Filter: DPENUMPLAYERS_LOCAL (8) or
 * _REMOTE (2) only; the callback's flags say which a player is (DPlayDLL takes the local one as
 * its own), plus server player and spectator from its creation flags. */
static void m_EnumPlayers(rc_cpu *c)
{
    net_poll();
    uint32_t cb = RC_ARG(c, 2), ctx = RC_ARG(c, 3), want = RC_ARG(c, 4);
    uint32_t name = rc_heap_alloc(16 + 2 * NET_NAME, true);
    for (int i = 0; i < NET_PLAYERS; i++) {
        net_player *p = net_player_at(i);
        if (!p || ((want & 8) && !p->local) || ((want & 2) && p->local)) {
            continue;
        }
        rc_st32(name, 16);
        memcpy(rc_ptr(name + 16), p->short_name, NET_NAME);
        memcpy(rc_ptr(name + 16 + NET_NAME), p->long_name, NET_NAME);
        rc_st32(name + 8, name + 16);
        rc_st32(name + 12, name + 16 + NET_NAME);
        uint32_t flags = (p->flags & 0x300u) | (p->local ? 8u : 2u);
        uint32_t args[5] = {p->id, 1 /* DPPLAYERTYPE_PLAYER */, name, flags, ctx};
        if (!rc_call_guest(c, cb, 5, args)) {
            break;
        }
    }
    rc_heap_free(name);
    RC_RETURN(c, DP_OK, 5);
}

/* GetPlayerName(this, id, data, size*): a DPNAME and its strings. */
static void m_GetPlayerName(rc_cpu *c)
{
    net_player *p = net_player_by_id(RC_ARG(c, 1));
    if (!p) {
        RC_RETURN(c, DPERR_INVALIDPLAYER, 4);
    }
    uint32_t data = RC_ARG(c, 2), psize = RC_ARG(c, 3);
    uint32_t ls = (uint32_t)strlen(p->short_name) + 1, ll = (uint32_t)strlen(p->long_name) + 1;
    uint32_t need = 16 + ls + ll, cap = rc_ld32(psize);
    rc_st32(psize, need);
    if (!data || cap < need) {
        RC_RETURN(c, DPERR_BUFFERTOOSMALL, 4);
    }
    rc_st32(data, 16);
    rc_st32(data + 4, 0);
    memcpy(rc_ptr(data + 16), p->short_name, ls);
    memcpy(rc_ptr(data + 16 + ls), p->long_name, ll);
    rc_st32(data + 8, data + 16);
    rc_st32(data + 12, data + 16 + ls);
    RC_RETURN(c, DP_OK, 4);
}

/* GetPlayerAddress(this, id, data, size*): the service provider and the player's IP address. */
static void m_GetPlayerAddress(rc_cpu *c)
{
    if (!net_player_by_id(RC_ARG(c, 1))) {
        RC_RETURN(c, DPERR_INVALIDPLAYER, 4);
    }
    char ip[64];
    net_player_ip(RC_ARG(c, 1), ip, sizeof ip);
    uint32_t il = (uint32_t)strlen(ip) + 1;
    uint32_t data = RC_ARG(c, 2), psize = RC_ARG(c, 3), need = 36 + 20 + il, cap = rc_ld32(psize);
    rc_st32(psize, need);
    if (!data || cap < need) {
        RC_RETURN(c, DPERR_BUFFERTOOSMALL, 4);
    }
    memcpy(rc_ptr(data), AID_SP, 16);
    rc_st32(data + 16, 16);
    memcpy(rc_ptr(data + 20), SP_TCPIP, 16);
    memcpy(rc_ptr(data + 36), AID_INET, 16);
    rc_st32(data + 52, il);
    memcpy(rc_ptr(data + 56), ip, il);
    RC_RETURN(c, DP_OK, 4);
}

/* GetSessionDesc(this, data, size*) */
static void m_GetSessionDesc(rc_cpu *c)
{
    const net_session *s = net_current();
    uint32_t data = RC_ARG(c, 1), psize = RC_ARG(c, 2);
    uint32_t need = 80 + (uint32_t)strnlen(s->name, NET_NAME - 1) + 1, cap = rc_ld32(psize);
    rc_st32(psize, need);
    if (!data || cap < need) {
        RC_RETURN(c, DPERR_BUFFERTOOSMALL, 3);
    }
    put_desc(data, s);
    RC_RETURN(c, DP_OK, 3);
}

/* SetSessionDesc(this, desc, flags): the host answers queries with it. */
static void m_SetSessionDesc(rc_cpu *c)
{
    net_session s;
    get_desc(RC_ARG(c, 1), &s);
    memcpy(s.app, net_current()->app, 16);
    net_set_session(&s);
    RC_RETURN(c, DP_OK, 3);
}

/* GetCaps(this, DPCAPS*, flags) */
static void m_GetCaps(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 1);
    memset(rc_ptr(p + 4), 0, 36);
    rc_st32(p + 4, 0x4 | 0x40 | (net_is_host() ? 0x2 : 0)); /* GUARANTEEDOPTIMIZED, ASYNCSUPPORTED, ISHOST */
    rc_st32(p + 8, 65536);                                   /* max buffer size */
    rc_st32(p + 16, 8);                                      /* max players */
    rc_st32(p + 20, 1000000);                                /* hundred baud */
    rc_st32(p + 24, 20);                                     /* latency, ms */
    rc_st32(p + 28, 8);                                      /* max local players */
    rc_st32(p + 36, 5000);                                   /* timeout, ms */
    RC_RETURN(c, DP_OK, 3);
}

static void m_GetPlayerCaps(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 2);
    memset(rc_ptr(p + 4), 0, 36);
    rc_st32(p + 8, 65536);
    rc_st32(p + 24, 20);
    RC_RETURN(c, DP_OK, 4);
}

static void m_GetPlayerFlags(rc_cpu *c)
{
    net_player *p = net_player_by_id(RC_ARG(c, 1));
    if (!p) {
        RC_RETURN(c, DPERR_INVALIDPLAYER, 3);
    }
    rc_st32(RC_ARG(c, 2), (p->flags & 0x300u) | (p->local ? 0x8u /* DPPLAYER_LOCAL */ : 0));
    RC_RETURN(c, DP_OK, 3);
}

static uint32_t next_group = 0x80000100u;
static void m_CreateGroup(rc_cpu *c)
{
    rc_st32(RC_ARG(c, 1), next_group++);
    RC_RETURN(c, DP_OK, 6);
}
static void ok2(rc_cpu *c) { RC_RETURN(c, DP_OK, 2); }
static void ok3(rc_cpu *c) { RC_RETURN(c, DP_OK, 3); }
static void ok4(rc_cpu *c) { RC_RETURN(c, DP_OK, 4); }
static void ok5(rc_cpu *c) { RC_RETURN(c, DP_OK, 5); }
static void ok6(rc_cpu *c) { RC_RETURN(c, DP_OK, 6); }

/* ---- IDirectPlayLobby3A: addresses only; the game is never started by a lobby. ---- */

static const char *const lobby_methods[] = {
    "QueryInterface", "AddRef", "Release", "Connect", "CreateAddress", "EnumAddress", "EnumAddressTypes",
    "EnumLocalApplications", "GetConnectionSettings", "ReceiveLobbyMessage", "RunApplication",
    "SendLobbyMessage", "SetConnectionSettings", "SetLobbyMessageEvent", "CreateCompoundAddress", "ConnectEx",
    "RegisterApplication", "UnregisterApplication", "WaitForConnectionSettings",
};

static uint32_t lobby_refs;

static uint32_t create_lobby(rc_cpu *c, uint32_t iid)
{
    (void)c;
    (void)iid;
    static uint32_t obj;
    if (!obj) {
        obj = rc_heap_alloc(8, true);
        rc_st32(obj, make_vtable("IDirectPlayLobby3A", LCOM, lobby_methods, sizeof lobby_methods / sizeof *lobby_methods));
    }
    lobby_refs++;
    return obj;
}

/* Write `count` address elements (guid, size, data) into the guest buffer if it fits. */
static uint32_t put_address(uint32_t out, uint32_t psize, uint32_t count, const uint32_t *guids,
                            const uint32_t *sizes, const uint32_t *datas)
{
    uint32_t need = 0;
    for (uint32_t i = 0; i < count; i++) {
        need += 20 + sizes[i];
    }
    uint32_t cap = rc_ld32(psize);
    rc_st32(psize, need);
    if (!out || cap < need) {
        return DPERR_BUFFERTOOSMALL;
    }
    for (uint32_t i = 0; i < count; i++) {
        memmove(rc_ptr(out), rc_ptr(guids[i]), 16);
        rc_st32(out + 16, sizes[i]);
        if (sizes[i]) {
            memmove(rc_ptr(out + 20), rc_ptr(datas[i]), sizes[i]);
        }
        out += 20 + sizes[i];
    }
    return DP_OK;
}

/* CreateAddress(this, sp guid, data type, data, size, address, size*) */
static void l_CreateAddress(rc_cpu *c)
{
    static uint32_t aid_sp;
    if (!aid_sp) {
        aid_sp = rc_heap_alloc(16, false);
        memcpy(rc_ptr(aid_sp), AID_SP, 16);
    }
    uint32_t guids[2] = {aid_sp, RC_ARG(c, 2)}, sizes[2] = {16, RC_ARG(c, 4)}, datas[2] = {RC_ARG(c, 1), RC_ARG(c, 3)};
    RC_RETURN(c, put_address(RC_ARG(c, 5), RC_ARG(c, 6), 2, guids, sizes, datas), 7);
}

/* CreateCompoundAddress(this, elements, count, address, size*): elements of guid, size, data*. */
static void l_CreateCompoundAddress(rc_cpu *c)
{
    uint32_t el = RC_ARG(c, 1), count = RC_ARG(c, 2);
    uint32_t guids[16], sizes[16], datas[16];
    if (count > 16) {
        RC_RETURN(c, DPERR_INVALIDPARAMS, 5);
    }
    for (uint32_t i = 0; i < count; i++) {
        guids[i] = el + 24 * i;
        sizes[i] = rc_ld32(el + 24 * i + 16);
        datas[i] = rc_ld32(el + 24 * i + 20);
    }
    RC_RETURN(c, put_address(RC_ARG(c, 3), RC_ARG(c, 4), count, guids, sizes, datas), 5);
}

/* EnumAddress(this, callback, address, size, context): callback(guid, size, data, context). */
static void l_EnumAddress(rc_cpu *c)
{
    uint32_t cb = RC_ARG(c, 1), a = RC_ARG(c, 2), size = RC_ARG(c, 3), ctx = RC_ARG(c, 4);
    for (uint32_t at = 0; at + 20 <= size;) {
        uint32_t len = rc_ld32(a + at + 16);
        uint32_t args[4] = {a + at, len, a + at + 20, ctx};
        if (!rc_call_guest(c, cb, 4, args)) {
            break;
        }
        at += 20 + len;
    }
    RC_RETURN(c, DP_OK, 5);
}

static void l_QueryInterface(rc_cpu *c)
{
    lobby_refs++;
    rc_st32(RC_ARG(c, 2), RC_ARG(c, 0));
    RC_RETURN(c, DP_OK, 3);
}
/* DPlayDLL takes a non-zero Release result for an error: the count must be real. */
static void l_AddRef(rc_cpu *c) { RC_RETURN(c, ++lobby_refs, 1); }
static void l_Release(rc_cpu *c) { RC_RETURN(c, lobby_refs ? --lobby_refs : 0, 1); }
static void l_notlobbied4(rc_cpu *c) { RC_RETURN(c, DPERR_NOTLOBBIED, 4); }
static void l_notlobbied5(rc_cpu *c) { RC_RETURN(c, DPERR_NOTLOBBIED, 5); }
static void l_notlobbied6(rc_cpu *c) { RC_RETURN(c, DPERR_NOTLOBBIED, 6); }
static void l_RunApplication(rc_cpu *c) { RC_RETURN(c, DPERR_UNSUPPORTED, 5); }

static const rc_import_def defs[] = {
    {COM, "IDirectPlay4A::QueryInterface", m_QueryInterface},
    {COM, "IDirectPlay4A::AddRef", m_AddRef},
    {COM, "IDirectPlay4A::Release", m_Release},
    {COM, "IDirectPlay4A::EnumConnections", m_EnumConnections},
    {COM, "IDirectPlay4A::InitializeConnection", m_InitializeConnection},
    {COM, "IDirectPlay4A::EnumSessions", m_EnumSessions},
    {COM, "IDirectPlay4A::Open", m_Open},
    {COM, "IDirectPlay4A::SecureOpen", m_SecureOpen},
    {COM, "IDirectPlay4A::Close", m_Close},
    {COM, "IDirectPlay4A::CreatePlayer", m_CreatePlayer},
    {COM, "IDirectPlay4A::DestroyPlayer", m_DestroyPlayer},
    {COM, "IDirectPlay4A::Send", m_Send},
    {COM, "IDirectPlay4A::SendEx", m_SendEx},
    {COM, "IDirectPlay4A::Receive", m_Receive},
    {COM, "IDirectPlay4A::GetMessageCount", m_GetMessageCount},
    {COM, "IDirectPlay4A::GetMessageQueue", m_GetMessageQueue},
    {COM, "IDirectPlay4A::EnumPlayers", m_EnumPlayers},
    {COM, "IDirectPlay4A::GetPlayerName", m_GetPlayerName},
    {COM, "IDirectPlay4A::GetPlayerAddress", m_GetPlayerAddress},
    {COM, "IDirectPlay4A::GetSessionDesc", m_GetSessionDesc},
    {COM, "IDirectPlay4A::SetSessionDesc", m_SetSessionDesc},
    {COM, "IDirectPlay4A::GetCaps", m_GetCaps},
    {COM, "IDirectPlay4A::GetPlayerCaps", m_GetPlayerCaps},
    {COM, "IDirectPlay4A::GetPlayerFlags", m_GetPlayerFlags},
    {COM, "IDirectPlay4A::GetPlayerData", m_GetPlayerData},
    {COM, "IDirectPlay4A::SetPlayerData", m_SetPlayerData},
    {COM, "IDirectPlay4A::SetPlayerName", m_SetPlayerName},
    {COM, "IDirectPlay4A::CreateGroup", m_CreateGroup},
    {COM, "IDirectPlay4A::DestroyGroup", ok2},
    {COM, "IDirectPlay4A::AddPlayerToGroup", ok3},
    {COM, "IDirectPlay4A::DeletePlayerFromGroup", ok3},
    {COM, "IDirectPlay4A::EnumGroups", ok5},
    {COM, "IDirectPlay4A::EnumGroupPlayers", ok6},
    {COM, "IDirectPlay4A::EnumGroupsInGroup", ok6},
    {COM, "IDirectPlay4A::SetGroupData", ok5},
    {COM, "IDirectPlay4A::SetGroupName", ok4},
    {COM, "IDirectPlay4A::SendChatMessage", ok5},
    {COM, "IDirectPlay4A::StartSession", ok3},
    {COM, "IDirectPlay4A::CancelMessage", ok3},
    {COM, "IDirectPlay4A::CancelPriority", ok4},
    {COM, "IDirectPlay4A::Initialize", ok2},
    {LCOM, "IDirectPlayLobby3A::QueryInterface", l_QueryInterface},
    {LCOM, "IDirectPlayLobby3A::AddRef", l_AddRef},
    {LCOM, "IDirectPlayLobby3A::Release", l_Release},
    {LCOM, "IDirectPlayLobby3A::CreateAddress", l_CreateAddress},
    {LCOM, "IDirectPlayLobby3A::CreateCompoundAddress", l_CreateCompoundAddress},
    {LCOM, "IDirectPlayLobby3A::EnumAddress", l_EnumAddress},
    {LCOM, "IDirectPlayLobby3A::EnumAddressTypes", ok5},
    {LCOM, "IDirectPlayLobby3A::EnumLocalApplications", ok4},
    {LCOM, "IDirectPlayLobby3A::RegisterApplication", ok3},
    {LCOM, "IDirectPlayLobby3A::UnregisterApplication", ok3},
    {LCOM, "IDirectPlayLobby3A::GetConnectionSettings", l_notlobbied4},
    {LCOM, "IDirectPlayLobby3A::SetConnectionSettings", l_notlobbied4},
    {LCOM, "IDirectPlayLobby3A::SetLobbyMessageEvent", l_notlobbied4},
    {LCOM, "IDirectPlayLobby3A::Connect", l_notlobbied4},
    {LCOM, "IDirectPlayLobby3A::ConnectEx", l_notlobbied5},
    {LCOM, "IDirectPlayLobby3A::SendLobbyMessage", l_notlobbied5},
    {LCOM, "IDirectPlayLobby3A::ReceiveLobbyMessage", l_notlobbied6},
    {LCOM, "IDirectPlayLobby3A::RunApplication", l_RunApplication},
    {LCOM, "IDirectPlayLobby3A::WaitForConnectionSettings", ok2},
};

/* The network is serviced at every message pump too, so a host answers joins and relays
 * messages while its game is busy elsewhere. */
static void pump(rc_cpu *c)
{
    (void)c;
    if (net_is_open()) {
        net_poll();
    }
}

void rc_dplay_init(void)
{
    rc_dpnet_init();
    rc_add_pump_hook(pump);
    rc_register_imports(defs, sizeof defs / sizeof defs[0]);
    rc_com_register("{D1EB6D20-8923-11D0-9D97-00A0C90A43CB}", create);       /* CLSID_DirectPlay */
    rc_com_register("{2FE8F810-B2A5-11D0-A787-0000F803ABFC}", create_lobby); /* CLSID_DirectPlayLobby */
}
