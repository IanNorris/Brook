// runtime.cpp -- Compiler runtime builtins for freestanding kernel.
//
// The compiler generates implicit calls to memset/memcpy/memmove for
// struct initialization, large copies, etc.  In a freestanding environment
// we must provide them as linkable symbols.
//
// We use x86 `rep movsb` / `rep stosb` which modern CPUs (Ivy Bridge+)
// microcode into wide-store loops via ERMS (Enhanced REP MOVSB/STOSB).
// This beats hand-rolled qword loops by 2-5× for large copies and works
// regardless of alignment.  QEMU KVM passes through the host CPU's ERMS;
// QEMU TCG still benefits since rep movsb is a single instruction the
// translator can optimize.

#include <stdint.h>
#include <stddef.h>

extern "C" void* memset(void* s, int c, size_t n)
{
    void* ret = s;
    asm volatile("rep stosb"
                 : "+D"(s), "+c"(n)
                 : "a"(static_cast<unsigned char>(c))
                 : "memory");
    return ret;
}

extern "C" void* memcpy(void* __restrict__ dst, const void* __restrict__ src, size_t n)
{
    void* ret = dst;
    asm volatile("rep movsb"
                 : "+D"(dst), "+S"(src), "+c"(n)
                 :
                 : "memory");
    return ret;
}

extern "C" void* memmove(void* dst, const void* src, size_t n)
{
    auto* d = static_cast<unsigned char*>(dst);
    auto* s = static_cast<const unsigned char*>(src);
    if (d < s) {
        asm volatile("rep movsb"
                     : "+D"(d), "+S"(s), "+c"(n)
                     :
                     : "memory");
    } else if (d > s) {
        // Backward copy: point to last byte, set direction flag
        d += n - 1;
        s += n - 1;
        asm volatile("std; rep movsb; cld"
                     : "+D"(d), "+S"(s), "+c"(n)
                     :
                     : "memory");
    }
    return dst;
}
