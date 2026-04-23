/*
 * net_brook.c — Network backend for Quake 2 on Brook OS.
 *
 * Uses real UDP sockets via the Brook/musl BSD socket API for NA_IP and
 * NA_BROADCAST traffic.  Keeps the original in-process loopback ring for
 * NA_LOOPBACK so single-player still works without touching the network.
 */

#include "qcommon.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ------------------------------------------------------------------------ */
/* Loopback path (single-player)                                             */
/* ------------------------------------------------------------------------ */

#define MAX_LOOPBACK     4
#define MAX_LOOPBACK_MSG 2048

typedef struct
{
    byte data[MAX_LOOPBACK_MSG];
    int  datalen;
} loopmsg_t;

typedef struct
{
    loopmsg_t msgs[MAX_LOOPBACK];
    int get, send;
} loopback_t;

static loopback_t loopbacks[2]; /* [NS_CLIENT], [NS_SERVER] */

static qboolean NET_GetLoopPacket(netsrc_t sock, netadr_t *net_from, sizebuf_t *net_message)
{
    loopback_t *loop = &loopbacks[sock];

    if (loop->send - loop->get > MAX_LOOPBACK)
        loop->get = loop->send - MAX_LOOPBACK;

    if (loop->get >= loop->send)
        return false;

    int i = loop->get & (MAX_LOOPBACK - 1);
    loop->get++;

    memcpy(net_message->data, loop->msgs[i].data, loop->msgs[i].datalen);
    net_message->cursize = loop->msgs[i].datalen;

    memset(net_from, 0, sizeof(*net_from));
    net_from->type = NA_LOOPBACK;
    return true;
}

static void NET_SendLoopPacket(netsrc_t sock, int length, void *data)
{
    /* Send to the OTHER side's queue */
    loopback_t *loop = &loopbacks[sock ^ 1];

    int i = loop->send & (MAX_LOOPBACK - 1);
    loop->send++;

    if (length > MAX_LOOPBACK_MSG)
        length = MAX_LOOPBACK_MSG;
    memcpy(loop->msgs[i].data, data, length);
    loop->msgs[i].datalen = length;
}

/* ------------------------------------------------------------------------ */
/* UDP sockets (NS_CLIENT, NS_SERVER)                                        */
/* ------------------------------------------------------------------------ */

static int udp_sockets[2] = { -1, -1 };   /* [NS_CLIENT], [NS_SERVER] */
static qboolean udp_initialised = false;

static int OpenUdp(unsigned short port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    /* Non-blocking so NET_GetPacket returns EAGAIN immediately. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    /* Enable broadcast sends (server list refresh, LAN announce). */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family      = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void SockaddrToNetadr(const struct sockaddr_in *s, netadr_t *a)
{
    memset(a, 0, sizeof(*a));
    a->type = NA_IP;
    unsigned long ip = ntohl(s->sin_addr.s_addr);
    a->ip[0] = (byte)((ip >> 24) & 0xff);
    a->ip[1] = (byte)((ip >> 16) & 0xff);
    a->ip[2] = (byte)((ip >>  8) & 0xff);
    a->ip[3] = (byte)(ip         & 0xff);
    a->port  = s->sin_port; /* keep network byte order; Q2 treats it opaquely */
}

static void NetadrToSockaddr(const netadr_t *a, struct sockaddr_in *s)
{
    memset(s, 0, sizeof(*s));
    s->sin_family = AF_INET;
    s->sin_port   = a->port; /* already network byte order */
    if (a->type == NA_BROADCAST) {
        s->sin_addr.s_addr = htonl(INADDR_BROADCAST);
    } else {
        unsigned long ip = ((unsigned long)a->ip[0] << 24) |
                           ((unsigned long)a->ip[1] << 16) |
                           ((unsigned long)a->ip[2] <<  8) |
                           ((unsigned long)a->ip[3]      );
        s->sin_addr.s_addr = htonl(ip);
    }
}

/* ------------------------------------------------------------------------ */
/* Public NET_* API                                                          */
/* ------------------------------------------------------------------------ */

void NET_Init(void)
{
    memset(loopbacks, 0, sizeof(loopbacks));
    udp_sockets[NS_CLIENT] = -1;
    udp_sockets[NS_SERVER] = -1;
    udp_initialised = false;
}

void NET_Shutdown(void)
{
    for (int i = 0; i < 2; i++) {
        if (udp_sockets[i] >= 0) {
            close(udp_sockets[i]);
            udp_sockets[i] = -1;
        }
    }
    udp_initialised = false;
}

/* Lazily open sockets when we first see multiplayer traffic. */
static void EnsureUdp(void)
{
    if (udp_initialised)
        return;
    udp_initialised = true;

    /* Server binds PORT_SERVER so peers can find it; client gets ephemeral. */
    udp_sockets[NS_SERVER] = OpenUdp(PORT_SERVER);
    udp_sockets[NS_CLIENT] = OpenUdp(0);
}void NET_Config(qboolean multiplayer)
{
    if (multiplayer)
        EnsureUdp();
}

qboolean NET_GetPacket(netsrc_t sock, netadr_t *net_from, sizebuf_t *net_message)
{
    /* Loopback first so single-player keeps working even with sockets open. */
    if (NET_GetLoopPacket(sock, net_from, net_message))
        return true;

    EnsureUdp();
    int fd = udp_sockets[sock];
    if (fd < 0)
        return false;

    struct sockaddr_in from;
    socklen_t fromLen = sizeof(from);
    int n = recvfrom(fd, net_message->data, net_message->maxsize, 0,
                     (struct sockaddr *)&from, &fromLen);
    if (n < 0)
        return false;  /* EAGAIN or error */

    net_message->cursize = n;
    SockaddrToNetadr(&from, net_from);
    return true;
}

void NET_SendPacket(netsrc_t sock, int length, void *data, netadr_t to)
{
    if (to.type == NA_LOOPBACK) {
        NET_SendLoopPacket(sock, length, data);
        return;
    }
    if (to.type != NA_IP && to.type != NA_BROADCAST)
        return;

    EnsureUdp();
    int fd = udp_sockets[sock];
    if (fd < 0)
        return;

    struct sockaddr_in dst;
    NetadrToSockaddr(&to, &dst);
    sendto(fd, data, length, 0, (struct sockaddr *)&dst, sizeof(dst));
}

qboolean NET_CompareAdr(netadr_t a, netadr_t b)
{
    if (a.type != b.type)
        return false;
    if (a.type == NA_LOOPBACK)
        return true;
    if (a.type == NA_IP || a.type == NA_BROADCAST)
        return a.port == b.port && memcmp(a.ip, b.ip, 4) == 0;
    return memcmp(&a, &b, sizeof(a)) == 0;
}

qboolean NET_CompareBaseAdr(netadr_t a, netadr_t b)
{
    if (a.type != b.type)
        return false;
    if (a.type == NA_LOOPBACK)
        return true;
    if (a.type == NA_IP || a.type == NA_BROADCAST)
        return memcmp(a.ip, b.ip, 4) == 0;
    return memcmp(&a, &b, sizeof(a)) == 0;
}

qboolean NET_IsLocalAddress(netadr_t adr)
{
    return adr.type == NA_LOOPBACK;
}

char *NET_AdrToString(netadr_t a)
{
    static char s[64];
    if (a.type == NA_LOOPBACK) {
        snprintf(s, sizeof(s), "loopback");
    } else if (a.type == NA_BROADCAST) {
        snprintf(s, sizeof(s), "BROADCAST:%d", ntohs(a.port));
    } else {
        snprintf(s, sizeof(s), "%d.%d.%d.%d:%d",
                 a.ip[0], a.ip[1], a.ip[2], a.ip[3], ntohs(a.port));
    }
    return s;
}

/* Parse "A.B.C.D", "A.B.C.D:port", "localhost", or "loopback".
 * Returns false if the string isn't a recognised address (which makes
 * Q2 print "Bad server address" to the console). */
qboolean NET_StringToAdr(char *s, netadr_t *a)
{
    memset(a, 0, sizeof(*a));

    if (!s || !*s)
        return false;

    if (!strcmp(s, "localhost") || !strcmp(s, "loopback")) {
        a->type = NA_LOOPBACK;
        return true;
    }

    /* Copy so we can split on ':' */
    char buf[64];
    size_t slen = strlen(s);
    if (slen >= sizeof(buf))
        return false;
    memcpy(buf, s, slen + 1);

    char *colon = strchr(buf, ':');
    unsigned short port = PORT_SERVER;
    if (colon) {
        *colon = '\0';
        long p = strtol(colon + 1, NULL, 10);
        if (p <= 0 || p > 65535)
            return false;
        port = (unsigned short)p;
    }

    /* inet_aton returns 0 on failure. */
    struct in_addr inaddr;
    if (inet_aton(buf, &inaddr) == 0)
        return false;

    unsigned long ip = ntohl(inaddr.s_addr);
    a->type  = NA_IP;
    a->ip[0] = (byte)((ip >> 24) & 0xff);
    a->ip[1] = (byte)((ip >> 16) & 0xff);
    a->ip[2] = (byte)((ip >>  8) & 0xff);
    a->ip[3] = (byte)(ip         & 0xff);
    a->port  = htons(port);
    return true;
}

void NET_Sleep(int msec)
{
    (void)msec;
}
