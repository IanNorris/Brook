#pragma once
// Minimal string.h shim for freestanding kernel use.
// Only declares what FatFS and other third-party C code needs.
// All implementations are compiler builtins — no libc required.
// Deliberately avoids including stddef.h (unavailable in bare-metal C mode).

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
    return __builtin_memset(s, c, n);
}

static inline void* memcpy(void* __restrict__ dst, const void* __restrict__ src, size_t n)
{
    return __builtin_memcpy(dst, src, n);
}

static inline void* memmove(void* dst, const void* src, size_t n)
{
    return __builtin_memmove(dst, src, n);
}

static inline int memcmp(const void* a, const void* b, size_t n)
{
    return __builtin_memcmp(a, b, n);
}

static inline size_t strlen(const char* s)
{
    return __builtin_strlen(s);
}

static inline int strcmp(const char* a, const char* b)
{
    return __builtin_strcmp(a, b);
}

static inline int strncmp(const char* a, const char* b, size_t n)
{
    return __builtin_strncmp(a, b, n);
}

static inline const char* strchr(const char* s, int c)
{
    return __builtin_strchr(s, c);
}

#ifdef __cplusplus
}
#endif
