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
    uint8_t val = static_cast<uint8_t>(c);

    // Fast path: qword fill for large aligned fills
    if (n >= 32 && (reinterpret_cast<uintptr_t>(p) & 7) == 0)
    {
        uint64_t fill = val;
        fill |= fill << 8;
        fill |= fill << 16;
        fill |= fill << 32;
        auto* p8 = reinterpret_cast<uint64_t*>(p);
        size_t qwords = n / 8;
        for (size_t i = 0; i < qwords; ++i) p8[i] = fill;
        size_t done = qwords * 8;
        for (size_t i = done; i < n; ++i) p[i] = val;
    }
    else
    {
        for (size_t i = 0; i < n; ++i) p[i] = val;
    }
    return s;
}

extern "C" void* memcpy(void* __restrict__ dst, const void* __restrict__ src, size_t n)
{
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);

    // Fast path: qword-at-a-time for large aligned copies
    if (n >= 32 &&
        ((reinterpret_cast<uintptr_t>(d) | reinterpret_cast<uintptr_t>(s)) & 7) == 0)
    {
        auto* d8 = reinterpret_cast<uint64_t*>(d);
        auto* s8 = reinterpret_cast<const uint64_t*>(s);
        size_t qwords = n / 8;
        for (size_t i = 0; i < qwords; ++i) d8[i] = s8[i];
        size_t done = qwords * 8;
        for (size_t i = done; i < n; ++i) d[i] = s[i];
    }
    else
    {
        for (size_t i = 0; i < n; ++i) d[i] = s[i];
    }
    return dst;
}

extern "C" void* memmove(void* dst, const void* src, size_t n)
{
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    if (d < s) {
        // Forward: use qword copy when aligned
        if (n >= 32 &&
            ((reinterpret_cast<uintptr_t>(d) | reinterpret_cast<uintptr_t>(s)) & 7) == 0)
        {
            auto* d8 = reinterpret_cast<uint64_t*>(d);
            auto* s8 = reinterpret_cast<const uint64_t*>(s);
            size_t qwords = n / 8;
            for (size_t i = 0; i < qwords; ++i) d8[i] = s8[i];
            size_t done = qwords * 8;
            for (size_t i = done; i < n; ++i) d[i] = s[i];
        } else {
            for (size_t i = 0; i < n; ++i) d[i] = s[i];
        }
    } else {
        // Backward: use qword copy when aligned
        if (n >= 32 &&
            ((reinterpret_cast<uintptr_t>(d) | reinterpret_cast<uintptr_t>(s)) & 7) == 0)
        {
            size_t tail = n & 7;
            for (size_t i = n; i > n - tail; --i) d[i - 1] = s[i - 1];
            auto* d8 = reinterpret_cast<uint64_t*>(d);
            auto* s8 = reinterpret_cast<const uint64_t*>(s);
            size_t qwords = (n - tail) / 8;
            for (size_t i = qwords; i > 0; --i) d8[i - 1] = s8[i - 1];
        } else {
            for (size_t i = n; i > 0; --i) d[i - 1] = s[i - 1];
        }
    }
    return dst;
}
