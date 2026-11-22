/* SPDX-License-Identifier: MIT */
/*
 * Winsock (wsock32.dll and ws2_32.dll, by name and by ordinal): a machine without a network.
 *
 * WSAStartup succeeds and the host has a name, so code that only asks (the C2 engine's lobby
 * set-up) goes on; sockets cannot be created (WSAENETDOWN), so every connection attempt fails
 * the way it fails offline. Network play between recompiled builds is milestone M12.
 */
#include "wsock.h"

#include <stdio.h>
#include <string.h>

#include "dplay/net.h"
#include "rc/process.h"
#include "rc/vm.h"

#define WS1 "wsock32.dll"
#define WS2 "ws2_32.dll"
#define INVALID_SOCKET 0xffffffffu
#define SOCKET_ERROR 0xffffffffu
#define WSAENETDOWN 10050u
#define WSAENOTSOCK 10038u
#define WSAHOST_NOT_FOUND 11001u

static uint32_t last_error;

/* WSAStartup(version, WSADATA*) */
static void k_WSAStartup(rc_cpu *c)
{
    uint32_t v = RC_ARG(c, 0), d = RC_ARG(c, 1);
    if (d) {
        memset(rc_ptr(d), 0, 400);
        rc_st16(d, (uint16_t)v);
        rc_st16(d + 2, 0x0202);
        rc_put_str(d + 4, 257, "WinSock 2.0");
        rc_put_str(d + 261, 129, "Offline");
        rc_st16(d + 390, 64); /* iMaxSockets */
    }
    RC_RETURN(c, 0, 2);
}

static void k_WSACleanup(rc_cpu *c) { RC_RETURN(c, 0, 0); }
static void k_WSAGetLastError(rc_cpu *c) { RC_RETURN(c, last_error, 0); }
static void k_WSASetLastError(rc_cpu *c)
{
    last_error = RC_ARG(c, 0);
    RC_RETURN(c, 0, 1);
}

static void k_socket(rc_cpu *c)
{
    last_error = WSAENETDOWN;
    RC_RETURN(c, INVALID_SOCKET, 3);
}

/* Calls on a socket, which cannot exist: n arguments. */
#define NOT_A_SOCKET(name, n)                                                                      \
    static void name(rc_cpu *c)                                                                    \
    {                                                                                              \
        last_error = WSAENOTSOCK;                                                                  \
        RC_RETURN(c, SOCKET_ERROR, n);                                                             \
    }
NOT_A_SOCKET(k_closesocket, 1)
NOT_A_SOCKET(k_accept, 3)
NOT_A_SOCKET(k_bind, 3)
NOT_A_SOCKET(k_connect, 3)
NOT_A_SOCKET(k_listen, 2)
NOT_A_SOCKET(k_recv, 4)
NOT_A_SOCKET(k_send, 4)
NOT_A_SOCKET(k_recvfrom, 6)
NOT_A_SOCKET(k_sendto, 6)
NOT_A_SOCKET(k_setsockopt, 5)
NOT_A_SOCKET(k_getsockopt, 5)
NOT_A_SOCKET(k_shutdown, 2)
NOT_A_SOCKET(k_ioctlsocket, 3)
NOT_A_SOCKET(k_getsockname, 3)

/* select(n, read, write, except, timeout): nothing is ever ready. */
static void k_select(rc_cpu *c)
{
    for (int i = 1; i <= 3; i++) {
        if (RC_ARG(c, i)) {
            rc_st32(RC_ARG(c, i), 0); /* fd_count */
        }
    }
    RC_RETURN(c, 0, 5);
}

/* __WSAFDIsSet(socket, fd_set*) */
static void k_WSAFDIsSet(rc_cpu *c)
{
    uint32_t s = RC_ARG(c, 0), set = RC_ARG(c, 1);
    uint32_t n = set ? rc_ld32(set) : 0;
    for (uint32_t i = 0; i < n && i < 64; i++) {
        if (rc_ld32(set + 4 + 4 * i) == s) {
            RC_RETURN(c, 1, 2);
        }
    }
    RC_RETURN(c, 0, 2);
}

static void k_htons(rc_cpu *c)
{
    uint32_t v = RC_ARG(c, 0) & 0xffff;
    RC_RETURN(c, ((v & 0xff) << 8) | (v >> 8), 1);
}

static void k_htonl(rc_cpu *c)
{
    uint32_t v = RC_ARG(c, 0);
    RC_RETURN(c, (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24), 1);
}

/* inet_addr("a.b.c.d"): network byte order, INADDR_NONE if malformed. */
static void k_inet_addr(rc_cpu *c)
{
    const char *s = rc_str(RC_ARG(c, 0));
    unsigned a, b, d, e;
    char tail;
    if (!s || sscanf(s, "%u.%u.%u.%u%c", &a, &b, &d, &e, &tail) != 4 || a > 255 || b > 255 || d > 255 ||
        e > 255) {
        RC_RETURN(c, 0xffffffffu, 1);
    }
    RC_RETURN(c, a | b << 8 | d << 16 | e << 24, 1);
}

/* inet_ntoa(in_addr): "a.b.c.d" in a buffer that lasts until the next call. */
static void k_inet_ntoa(rc_cpu *c)
{
    static uint32_t buf;
    if (!buf) {
        buf = rc_heap_alloc(16, true);
    }
    uint32_t a = RC_ARG(c, 0);
    char s[16];
    snprintf(s, sizeof s, "%u.%u.%u.%u", a & 0xff, a >> 8 & 0xff, a >> 16 & 0xff, a >> 24);
    rc_put_str(buf, 16, s);
    RC_RETURN(c, buf, 1);
}

/* gethostname(buffer, size) */
static void k_gethostname(rc_cpu *c)
{
    const char *name = "recomp";
    if (RC_ARG(c, 1) <= strlen(name)) {
        last_error = 10014; /* WSAEFAULT */
        RC_RETURN(c, SOCKET_ERROR, 2);
    }
    rc_put_str(RC_ARG(c, 0), RC_ARG(c, 1), name);
    RC_RETURN(c, 0, 2);
}

/* gethostbyname(name): this machine (its own name, as gethostname gives it, or "localhost"), whose
 * address programs show to the players that want to join; other names are not looked up (the
 * servers games of the time asked are gone). */
static void k_gethostbyname(rc_cpu *c)
{
    const char *name = RC_ARG(c, 0) ? rc_str(RC_ARG(c, 0)) : "";
    if (strcmp(name, "recomp") != 0 && strcmp(name, "localhost") != 0 && name[0]) {
        last_error = WSAHOST_NOT_FOUND;
        RC_RETURN(c, 0, 1);
    }
    /* HOSTENT: name, aliases, type (short), length (short), address list; then the strings,
     * lists and the address. */
    static uint32_t he;
    if (!he) {
        he = rc_proc_alloc(64);
    }
    uint32_t aliases = he + 16, list = he + 24, addr = he + 32, str = he + 40;
    rc_put_str(str, 16, "recomp");
    rc_st32(he, str);
    rc_st32(he + 4, aliases);
    rc_st16(he + 8, 2); /* AF_INET */
    rc_st16(he + 10, 4);
    rc_st32(he + 12, list);
    rc_st32(aliases, 0);
    rc_st32(list, addr);
    rc_st32(list + 4, 0);
    uint32_t ip = net_local_ipv4();
    for (int i = 0; i < 4; i++) {
        rc_st8(addr + (uint32_t)i, (uint8_t)(ip >> (8 * i)));
    }
    RC_RETURN(c, he, 1);
}

#define BOTH(ord1, ord2, name, fn)                                                                 \
    {WS1, name, fn}, {WS2, name, fn}, {WS1, "#" #ord1, fn}, {WS2, "#" #ord2, fn}

const rc_import_def rc_wsock_defs[] = {
    /* The ordinals of wsock32 (Winsock 1.1) and ws2_32 agree except at 10 to 12: wsock32 has
     * inet_addr, inet_ntoa, ioctlsocket there, ws2_32 ioctlsocket, inet_addr, inet_ntoa. */
    BOTH(1, 1, "accept", k_accept),
    BOTH(2, 2, "bind", k_bind),
    BOTH(3, 3, "closesocket", k_closesocket),
    BOTH(4, 4, "connect", k_connect),
    BOTH(6, 6, "getsockname", k_getsockname),
    BOTH(7, 7, "getsockopt", k_getsockopt),
    BOTH(8, 8, "htonl", k_htonl),
    BOTH(9, 9, "htons", k_htons),
    BOTH(12, 10, "ioctlsocket", k_ioctlsocket),
    BOTH(10, 11, "inet_addr", k_inet_addr),
    BOTH(11, 12, "inet_ntoa", k_inet_ntoa),
    BOTH(13, 13, "listen", k_listen),
    BOTH(14, 14, "ntohl", k_htonl),
    BOTH(15, 15, "ntohs", k_htons),
    BOTH(16, 16, "recv", k_recv),
    BOTH(17, 17, "recvfrom", k_recvfrom),
    BOTH(18, 18, "select", k_select),
    BOTH(19, 19, "send", k_send),
    BOTH(20, 20, "sendto", k_sendto),
    BOTH(21, 21, "setsockopt", k_setsockopt),
    BOTH(22, 22, "shutdown", k_shutdown),
    BOTH(23, 23, "socket", k_socket),
    BOTH(52, 52, "gethostbyname", k_gethostbyname),
    BOTH(57, 57, "gethostname", k_gethostname),
    BOTH(111, 111, "WSAGetLastError", k_WSAGetLastError),
    BOTH(112, 112, "WSASetLastError", k_WSASetLastError),
    BOTH(115, 115, "WSAStartup", k_WSAStartup),
    BOTH(116, 116, "WSACleanup", k_WSACleanup),
    BOTH(151, 151, "__WSAFDIsSet", k_WSAFDIsSet),
};
const uint32_t rc_wsock_count = sizeof rc_wsock_defs / sizeof rc_wsock_defs[0];
