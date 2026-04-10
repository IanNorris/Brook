#pragma once
// Minimal string.h shim for freestanding kernel use.
// Only declares what FatFS and other third-party C code needs.
//
// We use explicit byte loops instead of __builtin_mem* because Clang may
// emit SSE/AVX instructions for the builtins.  FatFS (and other C code)
// is not compiled with -mgeneral-regs-only, so the compiler could inline
// vector ops that fault with #UD if SSE context is unexpected.

#ifndef __SIZE_TYPE__
typedef unsigned long size_t;
#else
typedef __SIZE_TYPE__ size_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline void* memset(void* s, int c, size_t n)
{
    unsigned char* p = (unsigned char*)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static inline void* memcpy(void* __restrict__ dst, const void* __restrict__ src, size_t n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dst;
}

static inline void* memmove(void* dst, const void* src, size_t n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

static inline int memcmp(const void* a, const void* b, size_t n)
{
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;
    for (size_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i])
            return pa[i] < pb[i] ? -1 : 1;
    }
    return 0;
}

static inline size_t strlen(const char* s)
{
    const char* p = s;
    while (*p) ++p;
    return (size_t)(p - s);
}

static inline int strcmp(const char* a, const char* b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}

static inline int strncmp(const char* a, const char* b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i] || a[i] == '\0')
            return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

static inline const char* strchr(const char* s, int c)
{
    while (*s) {
        if (*s == (char)c) return s;
        ++s;
    }
    return (c == '\0') ? s : (const char*)0;
}

#ifdef __cplusplus
}
#endif
