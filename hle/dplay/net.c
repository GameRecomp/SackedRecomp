/* SPDX-License-Identifier: MIT */
/* The network under our DirectPlay 4 and 8: see net.h. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define CLOSESOCK closesocket
#define NONBLOCK(s)                                                                                                    \
    do {                                                                                                               \
        u_long one_ = 1;                                                                                               \
        ioctlsocket((s), FIONBIO, &one_);                                                                              \
    } while (0)
#define WOULDBLOCK() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#define NONBLOCK(s) fcntl((s), F_SETFL, fcntl((s), F_GETFL, 0) | O_NONBLOCK)
#define WOULDBLOCK() (errno == EAGAIN || errno == EWOULDBLOCK)
#endif

#include "rc/log.h"

#define DISCOVERY_PORT 47624
#define CONNS 16
#define FOUND 16
/* A player record on the wire: id, flags, short and long name, data size, data. */
#define REC (8 + 2 * NET_NAME + 4)
/* A session on the wire (discovery answer after "CDPR"): see put_session. */
#define SESS (16 + 16 + 4 + 4 + 4 + 2 + NET_NAME + 4 + NET_APPDATA)

/* Frames on the TCP connection: u32 length (of the type and payload), u8 type. */
enum { F_WELCOME = 1, F_CREATE_REQ, F_PLAYER_ID, F_PLAYER_NEW, F_PLAYER_GONE, F_DATA, F_DESTROY_REQ, F_PLAYER_SET, F_SESSION };

typedef struct conn {
    SOCKET s;
    uint8_t *in;
    uint32_t in_len, in_cap;
} conn;

static struct {
    bool ready, open, host;
    net_session session;
    net_player players[NET_PLAYERS];
    uint32_t next_id;
    SOCKET udp, listener;
    conn conns[CONNS]; /* host: its clients; client: conns[0] is the host */
    int nconns;
    uint32_t pending_id; /* client: a player waiting for its id */
    bool awaiting_id;    /* client: net_create_player waits for pending_id */
    bool welcomed;
} n;

static net_events ev;

static void get_session(const uint8_t *r, net_session *s, uint16_t *port);
static void put_session(uint8_t *r, const net_session *s, uint16_t port);
static uint16_t listen_port(void);

static struct {
    net_session s;
    struct sockaddr_in addr; /* the host's TCP address */
} found[FOUND];
static int nfound;
static SOCKET disc = INVALID_SOCKET;
static uint64_t disc_sent;
static char extra_host[NET_NAME];

void net_set_events(const net_events *e) { ev = *e; }

uint64_t net_ms(void)
{
#if defined(_WIN32)
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
#endif
}

void net_sleep(uint32_t ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
#endif
}

static void init(void)
{
    if (n.ready) {
        return;
    }
#if defined(_WIN32)
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    n.udp = n.listener = INVALID_SOCKET;
    n.ready = true;
}

/* The address of this machine others on the network reach it at (in network byte order): the
 * one a UDP socket would send from towards the internet (no packet is sent); 127.0.0.1 without a
 * network. */
uint32_t net_local_ipv4(void)
{
    init();
    uint32_t ip = htonl(0x7f000001u);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) {
        return ip;
    }
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808u);
    struct sockaddr_in me = {0};
    socklen_t len = sizeof me;
    if (connect(s, (struct sockaddr *)&to, sizeof to) == 0 && getsockname(s, (struct sockaddr *)&me, &len) == 0 &&
        me.sin_addr.s_addr != 0) {
        ip = me.sin_addr.s_addr;
    }
    CLOSESOCK(s);
    return ip;
}

/* ---- players ---- */

static void free_player(net_player *p)
{
    free(p->data);
    memset(p, 0, sizeof *p);
}

net_player *net_player_by_id(uint32_t id)
{
    for (int i = 0; i < NET_PLAYERS; i++) {
        if (n.players[i].used && n.players[i].id == id) {
            return &n.players[i];
        }
    }
    return NULL;
}

net_player *net_player_at(int i) { return i >= 0 && i < NET_PLAYERS && n.players[i].used ? &n.players[i] : NULL; }

int net_player_count(void)
{
    int k = 0;
    for (int i = 0; i < NET_PLAYERS; i++) {
        k += n.players[i].used;
    }
    return k;
}

static void set_data(net_player *p, const void *data, uint32_t size)
{
    free(p->data);
    p->data = NULL;
    p->data_size = 0;
    if (size && data && (p->data = malloc(size)) != NULL) {
        memcpy(p->data, data, size);
        p->data_size = size;
    }
}

/* A copy of `r` (a record read from the wire, or made locally) in the list. */
static net_player *add_player(const net_player *r, bool local, int conn_index)
{
    for (int i = 0; i < NET_PLAYERS; i++) {
        net_player *p = &n.players[i];
        if (!p->used) {
            free_player(p);
            p->used = true;
            p->local = local;
            p->id = r->id;
            p->flags = r->flags;
            p->conn = conn_index;
            memcpy(p->short_name, r->short_name, NET_NAME);
            memcpy(p->long_name, r->long_name, NET_NAME);
            set_data(p, r->data, r->data_size);
            return p;
        }
    }
    return NULL;
}

static uint8_t *pack_player(const net_player *p, uint32_t *len)
{
    uint8_t *out = malloc(REC + p->data_size);
    *len = 0;
    if (!out) {
        return NULL;
    }
    memcpy(out, &p->id, 4);
    memcpy(out + 4, &p->flags, 4);
    memcpy(out + 8, p->short_name, NET_NAME);
    memcpy(out + 8 + NET_NAME, p->long_name, NET_NAME);
    memcpy(out + 8 + 2 * NET_NAME, &p->data_size, 4);
    if (p->data_size) {
        memcpy(out + REC, p->data, p->data_size);
    }
    *len = REC + p->data_size;
    return out;
}

/* Read a record into `p` (its data borrowed from `in`); its length, 0 if `len` is too short. */
static uint32_t unpack_player(const uint8_t *in, uint32_t len, net_player *p)
{
    uint32_t ds;
    if (len < REC) {
        return 0;
    }
    memcpy(&ds, in + 8 + 2 * NET_NAME, 4);
    if (len - REC < ds) {
        return 0;
    }
    memset(p, 0, sizeof *p);
    memcpy(&p->id, in, 4);
    memcpy(&p->flags, in + 4, 4);
    memcpy(p->short_name, in + 8, NET_NAME);
    memcpy(p->long_name, in + 8 + NET_NAME, NET_NAME);
    p->short_name[NET_NAME - 1] = p->long_name[NET_NAME - 1] = 0;
    p->data = (uint8_t *)(uintptr_t)(in + REC);
    p->data_size = ds;
    return REC + ds;
}

/* ---- connections ---- */

static void send_frame(conn *k, uint8_t type, const void *data, uint32_t size)
{
    uint8_t head[5];
    uint32_t len = size + 1;
    memcpy(head, &len, 4);
    head[4] = type;
    send(k->s, (const char *)head, 5, 0);
    if (size) {
        send(k->s, (const char *)data, (int)size, 0);
    }
}

static void broadcast(uint8_t type, const void *data, uint32_t size, int except)
{
    for (int i = 0; i < n.nconns; i++) {
        if (i != except && n.conns[i].s != INVALID_SOCKET) {
            send_frame(&n.conns[i], type, data, size);
        }
    }
}

static void broadcast_player(uint8_t type, const net_player *p, int except)
{
    uint32_t len;
    uint8_t *rec = pack_player(p, &len);
    if (rec) {
        broadcast(type, rec, len, except);
        free(rec);
    }
}

/* Deliver a message here (to a local player, or to everyone but the sender) and, on the host,
 * pass it to the connections that own its target. */
static void route(uint32_t from, uint32_t to, const uint8_t *data, uint32_t size, int from_conn)
{
    bool here = false;
    for (int i = 0; i < NET_PLAYERS && !here; i++) {
        net_player *p = &n.players[i];
        here = p->used && p->local && (to ? p->id == to : p->id != from);
    }
    if (here && ev.receive) {
        ev.receive(from, to, data, size);
    }
    if (!n.host) {
        return;
    }
    uint8_t *f = malloc(size + 8);
    if (!f) {
        return;
    }
    memcpy(f, &from, 4);
    memcpy(f + 4, &to, 4);
    memcpy(f + 8, data, size);
    for (int i = 0; i < n.nconns; i++) {
        if (i == from_conn || n.conns[i].s == INVALID_SOCKET) {
            continue;
        }
        bool wanted = to == 0;
        for (int k = 0; !wanted && k < NET_PLAYERS; k++) {
            wanted = n.players[k].used && n.players[k].id == to && n.players[k].conn == i;
        }
        if (wanted) {
            send_frame(&n.conns[i], F_DATA, f, size + 8);
        }
    }
    free(f);
}

static void gone(net_player *p)
{
    if (ev.player_gone) {
        ev.player_gone(p);
    }
    free_player(p);
}

static void handle_frame(int ci, uint8_t type, const uint8_t *p, uint32_t len)
{
    net_player r;
    uint32_t id;
    switch (type) {
    case F_CREATE_REQ: /* host: a client's new player */
        if (n.host && unpack_player(p, len, &r)) {
            r.id = n.next_id++;
            net_player *np = add_player(&r, false, ci);
            if (np) {
                send_frame(&n.conns[ci], F_PLAYER_ID, &np->id, 4);
                broadcast_player(F_PLAYER_NEW, np, ci);
                if (ev.player_new) {
                    ev.player_new(np, false);
                }
            }
        }
        break;
    case F_DESTROY_REQ:
        if (n.host && len >= 4) {
            memcpy(&id, p, 4);
            net_player *q = net_player_by_id(id);
            if (q) {
                broadcast(F_PLAYER_GONE, &id, 4, ci);
                gone(q);
            }
        }
        break;
    case F_WELCOME: /* client: the session's players */
        for (uint32_t o = 0, k; o < len && (k = unpack_player(p + o, len - o, &r)) != 0; o += k) {
            if (!net_player_by_id(r.id)) {
                net_player *np = add_player(&r, false, 0);
                if (np && ev.player_new) {
                    ev.player_new(np, true);
                }
            }
        }
        n.welcomed = true;
        break;
    case F_PLAYER_ID:
        if (len >= 4) {
            memcpy(&n.pending_id, p, 4);
        }
        break;
    case F_PLAYER_NEW:
        if (unpack_player(p, len, &r) && !net_player_by_id(r.id)) {
            net_player *np = add_player(&r, false, 0);
            if (np && ev.player_new) {
                ev.player_new(np, false);
            }
        }
        break;
    case F_PLAYER_SET: /* new names and data: keep them, pass them on (host), tell the game */
        if (unpack_player(p, len, &r)) {
            net_player *q = net_player_by_id(r.id);
            if (q && !q->local) {
                memcpy(q->short_name, r.short_name, NET_NAME);
                memcpy(q->long_name, r.long_name, NET_NAME);
                set_data(q, r.data, r.data_size);
                if (n.host) {
                    broadcast_player(F_PLAYER_SET, q, ci);
                }
                if (ev.player_data) {
                    ev.player_data(q);
                }
            }
        }
        break;
    case F_PLAYER_GONE:
        if (len >= 4) {
            memcpy(&id, p, 4);
            net_player *q = net_player_by_id(id);
            if (q) {
                gone(q);
            }
        }
        break;
    case F_SESSION: /* client: the host changed the session */
        if (!n.host && len >= SESS) {
            uint16_t port;
            get_session(p, &n.session, &port);
            if (ev.session_changed) {
                ev.session_changed(&n.session);
            }
        }
        break;
    case F_DATA:
        if (len >= 8) {
            uint32_t from, to;
            memcpy(&from, p, 4);
            memcpy(&to, p + 4, 4);
            route(from, to, p + 8, len - 8, ci);
        }
        break;
    default:
        break;
    }
}

/* While a new local player waits for its id, frames after the one that brings it wait too:
 * data for that player, sent by the host right after, must find it made. */
static bool held(void) { return n.awaiting_id && n.pending_id; }

/* Handle the whole frames in a connection's input. */
static void drop_conn(int ci);
static void take_frames(int ci)
{
    conn *k = &n.conns[ci];
    uint32_t at = 0;
    while (k->s != INVALID_SOCKET && !held() && k->in_len - at >= 5) {
        uint32_t len;
        memcpy(&len, k->in + at, 4);
        if (len == 0 || k->in_len - at - 4 < len) {
            break;
        }
        handle_frame(ci, k->in[at + 4], k->in + at + 5, len - 1);
        at += 4 + len;
    }
    if (k->s != INVALID_SOCKET && at) {
        memmove(k->in, k->in + at, k->in_len - at);
        k->in_len -= at;
    }
}

static void drop_conn(int ci)
{
    conn *k = &n.conns[ci];
    CLOSESOCK(k->s);
    k->s = INVALID_SOCKET;
    if (n.host) {
        for (int i = 0; i < NET_PLAYERS; i++) {
            net_player *p = &n.players[i];
            if (p->used && !p->local && p->conn == ci) {
                broadcast(F_PLAYER_GONE, &p->id, 4, ci);
                gone(p);
            }
        }
    } else {
        n.open = false;
        if (ev.session_lost) {
            ev.session_lost();
        }
    }
}

/* ---- sessions on the wire ---- */

static void put_session(uint8_t *r, const net_session *s, uint16_t port)
{
    memcpy(r, s->instance, 16);
    memcpy(r + 16, s->app, 16);
    memcpy(r + 32, &s->max_players, 4);
    memcpy(r + 36, &s->cur_players, 4);
    memcpy(r + 40, &s->flags, 4);
    memcpy(r + 44, &port, 2);
    memcpy(r + 46, s->name, NET_NAME);
    memcpy(r + 46 + NET_NAME, &s->app_data_size, 4);
    memcpy(r + 50 + NET_NAME, s->app_data, NET_APPDATA);
}

static void get_session(const uint8_t *r, net_session *s, uint16_t *port)
{
    memcpy(s->instance, r, 16);
    memcpy(s->app, r + 16, 16);
    memcpy(&s->max_players, r + 32, 4);
    memcpy(&s->cur_players, r + 36, 4);
    memcpy(&s->flags, r + 40, 4);
    memcpy(port, r + 44, 2);
    memcpy(s->name, r + 46, NET_NAME);
    s->name[NET_NAME - 1] = 0;
    memcpy(&s->app_data_size, r + 46 + NET_NAME, 4);
    if (s->app_data_size > NET_APPDATA) {
        s->app_data_size = NET_APPDATA;
    }
    memcpy(s->app_data, r + 50 + NET_NAME, NET_APPDATA);
}

static uint16_t listen_port(void)
{
    struct sockaddr_in la;
    socklen_t ll = sizeof la;
    if (n.listener != INVALID_SOCKET && getsockname(n.listener, (struct sockaddr *)&la, &ll) == 0) {
        return ntohs(la.sin_port);
    }
    return 0;
}

void net_poll(void)
{
    if (!n.ready) {
        return;
    }
    uint8_t buf[4096];
    if (n.host && n.udp != INVALID_SOCKET) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int k;
        while ((k = (int)recvfrom(n.udp, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl)) >= 4) {
            if (memcmp(buf, "CDPQ", 4) == 0) {
                uint8_t r[4 + SESS];
                n.session.cur_players = (uint32_t)net_player_count();
                memcpy(r, "CDPR", 4);
                put_session(r + 4, &n.session, listen_port());
                sendto(n.udp, (const char *)r, sizeof r, 0, (struct sockaddr *)&from, fl);
            }
            fl = sizeof from;
        }
    }
    if (n.host && n.listener != INVALID_SOCKET) {
        SOCKET s;
        while ((s = accept(n.listener, NULL, NULL)) != INVALID_SOCKET) {
            int ci = -1;
            for (int i = 0; i < n.nconns && ci < 0; i++) {
                if (n.conns[i].s == INVALID_SOCKET) {
                    ci = i;
                }
            }
            if (ci < 0 && n.nconns < CONNS) {
                ci = n.nconns++;
            }
            if (ci < 0) {
                CLOSESOCK(s);
                continue;
            }
            NONBLOCK(s);
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
            free(n.conns[ci].in);
            n.conns[ci] = (conn){.s = s};
            /* Welcome: the players so far. */
            uint8_t *all = NULL;
            uint32_t len = 0;
            for (int i = 0; i < NET_PLAYERS; i++) {
                uint32_t rl = 0;
                uint8_t *rec = n.players[i].used ? pack_player(&n.players[i], &rl) : NULL;
                uint8_t *grown = rec ? realloc(all, len + rl) : NULL;
                if (grown) {
                    all = grown;
                    memcpy(all + len, rec, rl);
                    len += rl;
                }
                free(rec);
            }
            send_frame(&n.conns[ci], F_WELCOME, all, len);
            free(all);
        }
    }
    for (int ci = 0; ci < n.nconns; ci++) {
        conn *k = &n.conns[ci];
        take_frames(ci); /* left from the last poll */
        while (k->s != INVALID_SOCKET && !held()) {
            int got = (int)recv(k->s, (char *)buf, sizeof buf, 0);
            if (got == 0 || (got < 0 && !WOULDBLOCK())) {
                drop_conn(ci);
                break;
            }
            if (got < 0) {
                break;
            }
            if (k->in_len + (uint32_t)got > k->in_cap) {
                k->in_cap = (k->in_len + (uint32_t)got) * 2;
                uint8_t *grown = realloc(k->in, k->in_cap);
                if (!grown) {
                    drop_conn(ci);
                    break;
                }
                k->in = grown;
            }
            memcpy(k->in + k->in_len, buf, (size_t)got);
            k->in_len += (uint32_t)got;
            take_frames(ci);
        }
    }
}

/* ---- discovery ---- */

void net_enum_start(const char *extra, bool forget)
{
    init();
    if (extra) {
        snprintf(extra_host, sizeof extra_host, "%s", extra);
    }
    if (forget) {
        nfound = 0;
    }
    if (disc == INVALID_SOCKET) {
        disc = socket(AF_INET, SOCK_DGRAM, 0);
        if (disc == INVALID_SOCKET) {
            return;
        }
        int one = 1;
        setsockopt(disc, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
        NONBLOCK(disc);
    }
    const char *targets[4] = {"255.255.255.255", "127.0.0.1", getenv("RC_DPLAY_HOST"), extra_host};
    for (int i = 0; i < 4; i++) {
        if (!targets[i] || !*targets[i]) {
            continue;
        }
        struct sockaddr_in to = {0};
        to.sin_family = AF_INET;
        to.sin_port = htons(DISCOVERY_PORT);
        if (inet_pton(AF_INET, targets[i], &to.sin_addr) != 1) {
            struct addrinfo hints = {0}, *res = NULL;
            hints.ai_family = AF_INET;
            if (getaddrinfo(targets[i], NULL, &hints, &res) != 0 || !res) {
                continue;
            }
            to.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
        sendto(disc, "CDPQ", 4, 0, (struct sockaddr *)&to, sizeof to);
    }
    disc_sent = net_ms();
}

void net_enum_poll(void)
{
    if (disc == INVALID_SOCKET) {
        return;
    }
    uint8_t r[4 + SESS + 64];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int k;
    while ((k = (int)recvfrom(disc, (char *)r, sizeof r, 0, (struct sockaddr *)&from, &fl)) >= 4 + SESS) {
        if (memcmp(r, "CDPR", 4) == 0) {
            net_session s;
            uint16_t port;
            get_session(r + 4, &s, &port);
            int i = 0;
            while (i < nfound && memcmp(found[i].s.instance, s.instance, 16) != 0) {
                i++;
            }
            if (i == nfound && nfound < FOUND) {
                nfound++;
            }
            if (i < nfound) {
                found[i].s = s;
                found[i].addr = from;
                found[i].addr.sin_port = htons(port);
            }
        }
        fl = sizeof from;
    }
}

bool net_enum_running(void) { return disc_sent != 0; }
uint64_t net_enum_started(void) { return disc_sent; }

void net_enum_stop(void)
{
    if (disc != INVALID_SOCKET) {
        CLOSESOCK(disc);
        disc = INVALID_SOCKET;
    }
    disc_sent = 0;
}

int net_sessions(const net_session **list)
{
    static net_session copy[FOUND];
    for (int i = 0; i < nfound; i++) {
        copy[i] = found[i].s;
    }
    *list = copy;
    return nfound;
}

bool net_session_host(const uint8_t instance[16], char *ip, int cap, uint16_t *port)
{
    for (int i = 0; i < nfound; i++) {
        if (memcmp(found[i].s.instance, instance, 16) == 0) {
            inet_ntop(AF_INET, &found[i].addr.sin_addr, ip, (socklen_t)cap);
            *port = ntohs(found[i].addr.sin_port);
            return true;
        }
    }
    return false;
}

/* ---- sessions ---- */

bool net_host(const net_session *s)
{
    init();
    net_enum_stop();
    net_close();
    n.session = *s;
    uint64_t t = net_ms();
    for (int i = 0; i < 16; i++) {
        n.session.instance[i] = (uint8_t)(rand() ^ (int)(t >> (i % 8 * 4)) ^ (int)(uintptr_t)&n >> i);
    }
    n.next_id = 0x100;
    n.udp = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(n.udp, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(DISCOVERY_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(n.udp, (struct sockaddr *)&a, sizeof a) != 0) {
        rc_log("[dplay] discovery port %d is taken: others will not find this game", DISCOVERY_PORT);
    }
    NONBLOCK(n.udp);
    n.listener = socket(AF_INET, SOCK_STREAM, 0);
    a.sin_port = 0;
    if (bind(n.listener, (struct sockaddr *)&a, sizeof a) != 0 || listen(n.listener, 8) != 0) {
        rc_log("[dplay] cannot listen for players");
        net_close();
        return false;
    }
    NONBLOCK(n.listener);
    n.host = n.open = true;
    return true;
}

bool net_join(const uint8_t instance[16])
{
    init();
    net_enum_poll();
    for (int i = 0; i < nfound; i++) {
        if (memcmp(found[i].s.instance, instance, 16) != 0) {
            continue;
        }
        net_session s = found[i].s;
        struct sockaddr_in addr = found[i].addr;
        net_enum_stop();
        net_close();
        SOCKET sk = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(sk, (struct sockaddr *)&addr, sizeof addr) != 0) {
            CLOSESOCK(sk);
            return false;
        }
        NONBLOCK(sk);
        int one = 1;
        setsockopt(sk, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        n.conns[0] = (conn){.s = sk};
        n.nconns = 1;
        n.host = false;
        n.session = s;
        n.open = true;
        n.welcomed = false;
        /* The welcome with the players so far. */
        for (uint64_t until = net_ms() + 3000; !n.welcomed && n.open && net_ms() < until;) {
            net_poll();
            net_sleep(5);
        }
        return n.open;
    }
    return false;
}

void net_set_session(const net_session *s)
{
    uint8_t keep[16];
    memcpy(keep, n.session.instance, 16);
    n.session = *s;
    memcpy(n.session.instance, keep, 16);
    if (n.host) {
        uint8_t r[SESS];
        n.session.cur_players = (uint32_t)net_player_count();
        put_session(r, &n.session, listen_port());
        broadcast(F_SESSION, r, sizeof r, -1);
    }
}

const net_session *net_current(void)
{
    n.session.cur_players = (uint32_t)net_player_count();
    return &n.session;
}

bool net_is_open(void) { return n.open; }
bool net_is_host(void) { return n.host; }

void net_close(void)
{
    for (int i = 0; i < n.nconns; i++) {
        if (n.conns[i].s != INVALID_SOCKET) {
            CLOSESOCK(n.conns[i].s);
        }
        free(n.conns[i].in);
    }
    if (n.udp != INVALID_SOCKET) {
        CLOSESOCK(n.udp);
    }
    if (n.listener != INVALID_SOCKET) {
        CLOSESOCK(n.listener);
    }
    for (int i = 0; i < NET_PLAYERS; i++) {
        free_player(&n.players[i]);
    }
    bool ready = n.ready;
    memset(&n, 0, sizeof n);
    n.ready = ready;
    n.udp = n.listener = INVALID_SOCKET;
}

/* ---- players and messages ---- */

uint32_t net_create_player(const char *short_name, const char *long_name, const void *data, uint32_t size,
                           uint32_t flags, uint32_t context)
{
    net_player r = {.flags = flags, .data = (uint8_t *)(uintptr_t)data, .data_size = data ? size : 0};
    snprintf(r.short_name, NET_NAME, "%s", short_name ? short_name : "");
    snprintf(r.long_name, NET_NAME, "%s", long_name ? long_name : "");
    if (!n.open) {
        return 0;
    }
    if (n.host) {
        r.id = n.next_id++;
    } else {
        uint32_t len;
        uint8_t *rec = pack_player(&r, &len);
        n.pending_id = 0;
        n.awaiting_id = true;
        if (rec) {
            send_frame(&n.conns[0], F_CREATE_REQ, rec, len);
            free(rec);
        }
        for (uint64_t until = net_ms() + 3000; !n.pending_id && n.open && net_ms() < until;) {
            net_poll();
            net_sleep(5);
        }
        n.awaiting_id = false;
        if (!n.pending_id) {
            return 0;
        }
        r.id = n.pending_id;
    }
    net_player *p = add_player(&r, true, n.host ? -1 : 0);
    if (!p) {
        return 0;
    }
    p->context = context;
    if (n.host) {
        broadcast_player(F_PLAYER_NEW, p, -1);
    }
    return p->id;
}

bool net_destroy_player(uint32_t id)
{
    net_player *p = net_player_by_id(id);
    if (!p) {
        return false;
    }
    free_player(p);
    if (n.host) {
        broadcast(F_PLAYER_GONE, &id, 4, -1);
    } else if (n.nconns && n.conns[0].s != INVALID_SOCKET) {
        send_frame(&n.conns[0], F_DESTROY_REQ, &id, 4);
    }
    return true;
}

bool net_set_player(uint32_t id, const char *short_name, const char *long_name, const void *data, uint32_t size,
                    bool keep_data, bool local_only)
{
    net_player *p = net_player_by_id(id);
    if (!p) {
        return false;
    }
    if (short_name) {
        snprintf(p->short_name, NET_NAME, "%s", short_name);
    }
    if (long_name) {
        snprintf(p->long_name, NET_NAME, "%s", long_name);
    }
    if (!keep_data) {
        set_data(p, data, size);
    }
    if (!local_only) {
        broadcast_player(F_PLAYER_SET, p, -1);
    }
    return true;
}

void net_player_ip(uint32_t id, char *out, int cap)
{
    snprintf(out, (size_t)cap, "127.0.0.1");
    net_player *p = net_player_by_id(id);
    if (p && n.host && !p->local && p->conn >= 0 && n.conns[p->conn].s != INVALID_SOCKET) {
        struct sockaddr_in a;
        socklen_t al = sizeof a;
        if (getpeername(n.conns[p->conn].s, (struct sockaddr *)&a, &al) == 0) {
            inet_ntop(AF_INET, &a.sin_addr, out, (socklen_t)cap);
        }
    }
}

void net_send(uint32_t from, uint32_t to, const void *data, uint32_t size)
{
    if (n.host) {
        route(from, to, data, size, -1);
        return;
    }
    net_player *p = to ? net_player_by_id(to) : NULL;
    if ((!to || !p || !p->local) && n.nconns && n.conns[0].s != INVALID_SOCKET) {
        uint8_t *f = malloc(size + 8);
        if (f) {
            memcpy(f, &from, 4);
            memcpy(f + 4, &to, 4);
            memcpy(f + 8, data, size);
            send_frame(&n.conns[0], F_DATA, f, size + 8);
            free(f);
        }
    }
    /* Local players other than the sender get theirs here. */
    bool here = false;
    for (int i = 0; i < NET_PLAYERS && !here; i++) {
        net_player *q = &n.players[i];
        here = q->used && q->local && (to ? q->id == to : q->id != from);
    }
    if (here && ev.receive) {
        ev.receive(from, to, data, size);
    }
}
