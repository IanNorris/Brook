// runtime.cpp -- Compiler runtime builtins for freestanding kernel.
//
// The compiler generates implicit calls to memset/memcpy/memmove for
// struct initialization, large copies, etc.  In a freestanding environment
// we must provide them as linkable symbols.

#include <stdint.h>
#include <stddef.h>

extern "C" void* memset(void* s, int c, size_t n)
{
    auto* p = static_cast<uint8_t*>(s);
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(c);
    return s;
}

extern "C" void* memcpy(void* __restrict__ dst, const void* __restrict__ src, size_t n)
{
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

extern "C" void* memmove(void* dst, const void* src, size_t n)
{
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    if (d < s)
        for (size_t i = 0; i < n; ++i) d[i] = s[i];
    else
        for (size_t i = n; i > 0; --i) d[i - 1] = s[i - 1];
    return dst;
}
