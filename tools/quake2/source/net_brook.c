/*
 * net_brook.c — Brook OS network for Quake 2
 *
 * Implements loopback networking for single-player.
 * Client and server communicate via in-memory ring buffers.
 */

#include "qcommon.h"

#include <string.h>
#include <stdio.h>

#define MAX_LOOPBACK 4
#define MAX_LOOPBACK_MSG 2048

typedef struct
{
    byte data[MAX_LOOPBACK_MSG];
    int datalen;
} loopmsg_t;

typedef struct
{
    loopmsg_t msgs[MAX_LOOPBACK];
    int get, send;
} loopback_t;

static loopback_t loopbacks[2]; /* [0] = client, [1] = server */

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

void NET_Init(void)
{
    memset(loopbacks, 0, sizeof(loopbacks));
}

void NET_Shutdown(void)
{
}

void NET_Config(qboolean multiplayer)
{
}

qboolean NET_GetPacket(netsrc_t sock, netadr_t *net_from, sizebuf_t *net_message)
{
    return NET_GetLoopPacket(sock, net_from, net_message);
}

void NET_SendPacket(netsrc_t sock, int length, void *data, netadr_t to)
{
    if (to.type == NA_LOOPBACK)
    {
        NET_SendLoopPacket(sock, length, data);
        return;
    }
    /* Drop non-loopback packets (no real network) */
}

qboolean NET_CompareAdr(netadr_t a, netadr_t b)
{
    if (a.type != b.type)
        return false;
    if (a.type == NA_LOOPBACK)
        return true;
    return memcmp(&a, &b, sizeof(a)) == 0;
}

qboolean NET_CompareBaseAdr(netadr_t a, netadr_t b)
{
    if (a.type != b.type)
        return false;
    if (a.type == NA_LOOPBACK)
        return true;
    return memcmp(&a, &b, sizeof(a)) == 0;
}

qboolean NET_IsLocalAddress(netadr_t adr)
{
    return adr.type == NA_LOOPBACK;
}

char *NET_AdrToString(netadr_t a)
{
    static char s[64];
    if (a.type == NA_LOOPBACK)
        snprintf(s, sizeof(s), "loopback");
    else
        snprintf(s, sizeof(s), "127.0.0.1:%d", a.port);
    return s;
}

qboolean NET_StringToAdr(char *s, netadr_t *a)
{
    memset(a, 0, sizeof(*a));

    if (!strcmp(s, "localhost") || !strcmp(s, "loopback"))
    {
        a->type = NA_LOOPBACK;
        return true;
    }

    return false;
}

void NET_Sleep(int msec)
{
}
