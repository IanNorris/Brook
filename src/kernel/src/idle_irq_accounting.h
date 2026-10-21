#pragma once
#include <stdint.h>

namespace brook {
// A local IRQ token, not a retained Process reference. Idle identities live
// for the CPU's lifetime. Disabling the mode cannot cancel an existing token.
inline bool IdleIrqAccountingContext(bool enabled,unsigned cpu,bool periodic,
                                     bool user,uintptr_t current,uintptr_t idle) {
    return enabled && cpu>0 && cpu<64 && periodic && !user && current && current==idle;
}
struct IdleIrqAccountingPending {
    unsigned cpu=0;
    uintptr_t identity=0;
    bool active=false;
    bool Begin(bool enabled,unsigned entryCpu,bool periodic,bool user,
               uintptr_t current,uintptr_t idle,uint64_t flags) {
        if(active || (flags&0x200) ||
           !IdleIrqAccountingContext(enabled,entryCpu,periodic,user,current,idle))return false;
        cpu=entryCpu;identity=current;active=true;return true;
    }
    bool Matches(unsigned nowCpu,uintptr_t current,uintptr_t idle,uint64_t flags) const {
        return active && !(flags&0x200) && nowCpu==cpu && current==identity && current==idle;
    }
    void Finish(){active=false;}
};
struct IdleIrqAccountingSnapshot {
    bool enabled=false;
    unsigned cpus=0;
    struct Cpu {
        uint64_t coalesced=0,lateSamples=0,fallbacks=0,identityErrors=0,ifErrors=0,lateWatchTicks=0;
        uint64_t lateNoNext=0,lateDispatch=0;
    } cpu[64]{};
};
void SchedulerIdleIrqAccountingMode(bool enabled);
void SchedulerIdleIrqAccountingRead(IdleIrqAccountingSnapshot* output);
}
