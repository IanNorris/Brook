/*
 * q_shbrook.c — Brook OS shared platform functions for Quake 2
 *
 * Provides Hunk memory management (mmap-based) and strlwr.
 * Replaces q_shlinux.c.
 */

#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/mman.h>

#include "qcommon.h"

// ---- Hunk memory (mmap-based) ----

byte *membase;
int maxhunksize;
int curhunksize;

void *Hunk_Begin(int maxsize)
{
    maxhunksize = maxsize + sizeof(int);
    curhunksize = 0;
    membase = mmap(0, maxhunksize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (membase == NULL || membase == (byte *)-1)
        Sys_Error("unable to virtual allocate %d bytes", maxsize);

    *((int *)membase) = curhunksize;
    return membase + sizeof(int);
}

void *Hunk_Alloc(int size)
{
    byte *buf;

    size = (size + 31) & ~31;
    if (curhunksize + size > maxhunksize)
        Sys_Error("Hunk_Alloc overflow");
    buf = membase + sizeof(int) + curhunksize;
    curhunksize += size;
    return buf;
}

int Hunk_End(void)
{
    // Brook may not support mremap — just keep the full allocation
    *((int *)membase) = curhunksize + sizeof(int);
    return curhunksize;
}

void Hunk_Free(void *base)
{
    if (base)
    {
        byte *m = ((byte *)base) - sizeof(int);
        munmap(m, *((int *)m));
    }
}

// ---- String helpers ----

char *strlwr(char *s)
{
    char *p = s;
    while (*p) { *p = tolower((unsigned char)*p); p++; }
    return s;
}
