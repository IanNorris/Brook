#include "vmware_clock.h"
#include "vmware_clock_policy.h"
#include "vmware_platform.h"
#include "serial.h"

namespace brook {
static uint32_t g_tscKhz;
bool VmwareClockInit()
{
    auto khz=VmwareTscFrequencyKhz(
        [](uint32_t leaf,uint32_t& a,uint32_t& b,uint32_t& c,uint32_t& d){
            __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(leaf),"c"(0));
        });
    if(khz && !VmwareEnableLazyTimers(
        VmwareBackdoor)) {
        SerialPuts("VMware clock: lazy-timer notification rejected; retaining IRQ fallback\n");
        khz=0;
    }
    __atomic_store_n(&g_tscKhz,khz,__ATOMIC_RELEASE);
    if(khz)SerialPrintf("VMware clock: lazy timers acknowledged, invariant virtual TSC %u kHz\n",khz);
    return khz!=0;
}
uint32_t VmwareClockTscKhz(){return __atomic_load_n(&g_tscKhz,__ATOMIC_ACQUIRE);}
bool VmwareClockReadTicks(uint64_t* ticks)
{
    if(!ticks || !VmwareClockTscKhz())return false;
    uint32_t lo,hi,aux;
    __asm__ volatile("rdtscp; lfence":"=a"(lo),"=d"(hi),"=c"(aux)::"memory");
    *ticks=(uint64_t(hi)<<32)|lo;
    return true;
}
}
