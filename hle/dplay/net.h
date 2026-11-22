/* SPDX-License-Identifier: MIT */
/*
 * The network under our DirectPlay 4 (dplay.c) and DirectPlay 8 (dpnet.c): sessions found on the
 * LAN, one host per session that numbers the players and relays their messages, players with
 * names and data. Recompiled programs talk to each other with it, not to Windows' DirectPlay.
 *
 * Discovery: a UDP query to port 47624 (broadcast, 127.0.0.1, $RC_DPLAY_HOST and an address the
 * player typed), answered by every host with its session. Play: one TCP connection from each
 * client to the host. Player ids come from the host, from 0x100 up.
 */
#ifndef RC_DPLAY_NET_H
#define RC_DPLAY_NET_H

#include <stdbool.h>
#include <stdint.h>

#define NET_NAME 64
#define NET_APPDATA 1024 /* Heroes IV keeps 304 bytes of game description here */

typedef struct net_session {
    uint8_t instance[16], app[16];
    uint32_t max_players, cur_players, flags;
    char name[NET_NAME];                 /* UTF-8 */
    uint8_t app_data[NET_APPDATA];       /* the front end's: DirectPlay 4's four user words, ... */
    uint32_t app_data_size;
} net_session;

typedef struct net_player {
    bool used, local;
    uint32_t id, flags;                  /* flags: the front end's, sent with the player */
    char short_name[NET_NAME], long_name[NET_NAME]; /* UTF-8 */
    uint8_t *data;                       /* player data, shared with every machine */
    uint32_t data_size;
    uint32_t context;                    /* the front end's, this machine only */
    int conn;                            /* host: the connection that owns it (-1: the host) */
} net_player;

/* What arrives, told to the front end (from net_poll and the calls that poll). */
typedef struct net_events {
    void (*player_new)(net_player *p, bool existing); /* existing: was there when we joined */
    void (*player_gone)(net_player *p);               /* before it is removed */
    void (*player_data)(net_player *p);               /* its names or data changed */
    void (*receive)(uint32_t from, uint32_t to, const uint8_t *data, uint32_t size);
    void (*session_lost)(void);
    void (*session_changed)(const net_session *s); /* client: the host changed it (may be NULL) */
} net_events;

void net_set_events(const net_events *ev);

/* Real time (the game's clock may be virtual). */
uint64_t net_ms(void);
void net_sleep(uint32_t ms);

/* Discovery. `extra` is one more address to ask (what the player typed), or NULL. Answers are
 * collected by net_enum_poll; net_enum_start again asks again (sessions found stay). */
void net_enum_start(const char *extra, bool forget);
void net_enum_poll(void);
bool net_enum_running(void);
uint64_t net_enum_started(void);
void net_enum_stop(void);
int net_sessions(const net_session **list);
/* Where a session found is hosted (address as text, TCP port); false if it is not known. */
bool net_session_host(const uint8_t instance[16], char *ip, int cap, uint16_t *port);

/* Host a session (instance GUID made here), or join one that was found. */
bool net_host(const net_session *s);
bool net_join(const uint8_t instance[16]);
void net_set_session(const net_session *s); /* host: answers queries with it, clients are told */
const net_session *net_current(void);
bool net_is_open(void);
bool net_is_host(void);
void net_close(void);

/* Players. net_create_player: the host numbers it (a client waits up to 3 s); 0 on failure. */
uint32_t net_create_player(const char *short_name, const char *long_name, const void *data, uint32_t size,
                           uint32_t flags, uint32_t context);
bool net_destroy_player(uint32_t id);
/* New names (NULL: keep) and data (keep_data: keep), sent to every machine unless local_only. */
bool net_set_player(uint32_t id, const char *short_name, const char *long_name, const void *data, uint32_t size,
                    bool keep_data, bool local_only);
net_player *net_player_by_id(uint32_t id);
net_player *net_player_at(int index); /* NULL for a free slot; index < NET_PLAYERS */
int net_player_count(void);
#define NET_PLAYERS 64
/* The address a player plays from, as text (the host knows its clients'). */
void net_player_ip(uint32_t id, char *out, int cap);

/* A message to one player or (to = 0) to everyone else. */
void net_send(uint32_t from, uint32_t to, const void *data, uint32_t size);

/* This machine's address on the network (network byte order; 127.0.0.1 without one). */
uint32_t net_local_ipv4(void);

/* Take what arrived (also done at every message pump while a session is open). */
void net_poll(void);

#endif
