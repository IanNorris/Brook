#pragma once
#include <stdint.h>

namespace brook {
template<class Cpuid> bool VmwarePlatformPresent(Cpuid cpuid)
{
    uint32_t a,b,c,d;
    cpuid(1,a,b,c,d);
    if (!(c & (1u<<31))) return false;
    cpuid(0x40000000u,a,b,c,d);
    return a>=0x40000000u && b==0x61774d56u && c==0x4d566572u && d==0x65726177u;
}

// Only call after exact VMware platform detection. Legacy I/O is supported by
// VMware alongside its newer hypercall instructions; all operands are 32-bit.
inline void VmwareBackdoor(uint32_t& a,uint32_t& b,uint32_t& c,uint32_t& d)
{
    __asm__ volatile("inl %%dx,%%eax":"+a"(a),"+b"(b),"+c"(c),"+d"(d)::"rsi","rdi","cc","memory");
}
}
