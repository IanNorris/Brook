/*
 * net_brook.c — Brook OS network stub for Quake 2
 *
 * Provides minimal net interface for single-player only.
 * No actual networking — just loopback.
 */

#include "qcommon.h"

void NET_Init(void)
{
}

void NET_Shutdown(void)
{
}

void NET_Config(qboolean multiplayer)
{
}

qboolean NET_GetPacket(netsrc_t sock, netadr_t *net_from, sizebuf_t *net_message)
{
    return false;
}

void NET_SendPacket(netsrc_t sock, int length, void *data, netadr_t to)
{
}

qboolean NET_CompareAdr(netadr_t a, netadr_t b)
{
    return memcmp(&a, &b, sizeof(a)) == 0;
}

qboolean NET_CompareBaseAdr(netadr_t a, netadr_t b)
{
    return NET_CompareAdr(a, b);
}

qboolean NET_IsLocalAddress(netadr_t adr)
{
    return true;
}

char *NET_AdrToString(netadr_t a)
{
    static char s[64];
    snprintf(s, sizeof(s), "127.0.0.1:%d", a.port);
    return s;
}

qboolean NET_StringToAdr(char *s, netadr_t *a)
{
    memset(a, 0, sizeof(*a));
    return false;
}

void NET_Sleep(int msec)
{
}
