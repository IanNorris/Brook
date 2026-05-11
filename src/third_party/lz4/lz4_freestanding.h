// Bridge header for LZ4 freestanding mode — provides memcpy/memset/memmove
// declarations that the kernel's runtime.cpp implements.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* dest, int c, size_t n);
void* memmove(void* dest, const void* src, size_t n);

#ifdef __cplusplus
}
#endif
