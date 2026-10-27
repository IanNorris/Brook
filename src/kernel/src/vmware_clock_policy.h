#pragma once
#include <stdint.h>

namespace brook {
// Vendor lazy-timer notification: do not hold virtual time back to replay
// periodic IRQs once the guest uses an independent elapsed source.
// ABI: CentOS 5.11 kernel-2.6.18-398.el5.src.rpm, kernel-2.6.18-redhat.patch,
// vmi_time_init(): command 49, EBX=-1, returned EBX must acknowledge the magic.
template<class Backdoor> bool VmwareEnableLazyTimers(Backdoor call)
{
    uint32_t a=0x564d5868u,b=~uint32_t(0),c=49,d=0x5658;
    call(a,b,c,d);
    return b==0x564d5868u;
}

template<class Cpuid> uint32_t VmwareTscFrequencyKhz(Cpuid cpuid)
{
    uint32_t a,b,c,d;
    cpuid(1,a,b,c,d);
    if (!(c & (1u<<31)) || !(d & (1u<<4))) return 0;
    cpuid(0x40000000u,a,b,c,d);
    if (a<0x40000010u || b!=0x61774d56u || c!=0x4d566572u || d!=0x65726177u) return 0;
    cpuid(0x80000000u,a,b,c,d);
    if (a<0x80000007u) return 0;
    cpuid(0x80000001u,a,b,c,d);
    if (!(d & (1u<<27))) return 0; // Ordered RDTSCP reads.
    cpuid(0x80000007u,a,b,c,d);
    if (!(d & (1u<<8))) return 0; // Conservative invariant-TSC gate.
    cpuid(0x40000010u,a,b,c,d);
    return a; // VMware hypervisor frequency leaf: kHz, not Hz.
}

inline bool VmwareTscElapsedMs(uint64_t originTsc,uint64_t originMs,
                               uint64_t nowTsc,uint32_t khz,uint64_t* output)
{
    if (!output || !khz || nowTsc<originTsc) return false;
    const uint64_t elapsed=(nowTsc-originTsc)/khz;
    if (originMs>~uint64_t(0)-elapsed) return false;
    *output=originMs+elapsed;
    return true;
}
}
