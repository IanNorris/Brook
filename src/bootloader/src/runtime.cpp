// Minimal C runtime stubs required by the compiler for freestanding UEFI builds.
// The compiler emits calls to memset/memcpy for aggregate zero-initialization and copies.
#include <Uefi.h>

extern "C"
{

void* memset(void* dst, int c, UINTN n)
{
    unsigned char* p = (unsigned char*)dst;
    while (n--)
    {
        *p++ = (unsigned char)c;
    }
    return dst;
}

void* memcpy(void* dst, const void* src, UINTN n)
{
    unsigned char*       d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--)
    {
        *d++ = *s++;
    }
    return dst;
}

void* memmove(void* dst, const void* src, UINTN n)
{
    if (reinterpret_cast<UINTN>(dst) <= reinterpret_cast<UINTN>(src)) return memcpy(dst, src, n);
    auto* d = static_cast<unsigned char*>(dst);
    const auto* s = static_cast<const unsigned char*>(src);
    while (n) {
        --n;
        d[n] = s[n];
    }
    return dst;
}

} // extern "C"
