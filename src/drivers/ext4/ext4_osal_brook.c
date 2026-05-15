// ext4_osal_brook.c — lwext4 OS abstraction layer for Brook kernel
//
// Provides the functions lwext4 expects from a hosted C environment:
//   - ext4_user_malloc/calloc/realloc/free (CONFIG_USE_USER_MALLOC=1)
//   - printf/fflush (used by ext4_debug.h when CONFIG_DEBUG_PRINTF=1)
//
// This file is compiled as C (not C++) because lwext4 is a C library.
// It calls kernel functions via their extern "C" ABI.

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "string.h"

// Kernel heap functions (exported via ksymtab)
extern void* kmalloc(uint64_t size);
extern void  kfree(void* ptr);
extern void* krealloc(void* ptr, uint64_t newSize);

// Kernel serial printf (exported via ksymtab)
extern int SerialPrintf(const char* fmt, ...);
extern int SerialVPrintf(const char* fmt, va_list ap);

// ---- Memory allocation for lwext4 (CONFIG_USE_USER_MALLOC) ----

void* ext4_user_malloc(size_t size)
{
    return kmalloc((uint64_t)size);
}

void* ext4_user_calloc(size_t nmemb, size_t size)
{
    uint64_t total = (uint64_t)nmemb * (uint64_t)size;
    void* p = kmalloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void* ext4_user_realloc(void* ptr, size_t size)
{
    return krealloc(ptr, (uint64_t)size);
}

void ext4_user_free(void* ptr)
{
    kfree(ptr);
}

// ---- printf/fflush for lwext4 debug output ----

int printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = SerialVPrintf(fmt, ap);
    va_end(ap);
    return ret;
}

int fflush(void* stream)
{
    (void)stream;
    return 0;  // serial output is unbuffered
}
