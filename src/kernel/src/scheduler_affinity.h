#pragma once
#include <stdint.h>
namespace brook {
// The idle task is CPU-local infrastructure and remains eligible everywhere.
// All other tasks, including kernel threads, honor an explicit CPU pin.
inline bool SchedulerCpuEligible(uint32_t pid,int32_t affinity,uint32_t cpu) {
    return pid==0 || affinity<0 || uint32_t(affinity)==cpu;
}
}
