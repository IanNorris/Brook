#include "mapping_access_timing.h"
#include "spinlock.h"
#include "kvmclock.h"

namespace brook {
uint64_t g_mappingAccessTimingEpoch=0;
static MappingAccessTimingModel g_accessTiming;
static SpinLock g_accessTimingLock;
uint64_t MappingAccessTimingClock(){uint64_t ns=0;return KvmClockReadStableNs(&ns)?ns:0;}
void MappingAccessTimingRecord(uint64_t token,bool write,uint64_t before,uint64_t acquired,
                              uint64_t ended,uint16_t pid,uint64_t incarnation){
    // Never hold AS.mutation while acquiring this diagnostic lock. No clock
    // reads, allocation, logging or scheduler calls occur under it.
    SpinLockAcquire(&g_accessTimingLock);
    g_accessTiming.Observe(token,write,before,acquired,ended,pid,incarnation);
    SpinLockRelease(&g_accessTimingLock);
}
void MappingAccessTimingEnable(bool enabled){
    SpinLockAcquire(&g_accessTimingLock);g_accessTiming.Enable(enabled);
    __atomic_store_n(&g_mappingAccessTimingEpoch,g_accessTiming.Token(),__ATOMIC_RELEASE);
    SpinLockRelease(&g_accessTimingLock);
}
void MappingAccessTimingReset(){
    SpinLockAcquire(&g_accessTimingLock);g_accessTiming.Reset();
    __atomic_store_n(&g_mappingAccessTimingEpoch,g_accessTiming.Token(),__ATOMIC_RELEASE);
    SpinLockRelease(&g_accessTimingLock);
}
MappingAccessTimingSnapshot MappingAccessTimingRead(){
    SpinLockAcquire(&g_accessTimingLock);auto result=g_accessTiming.Snapshot();
    SpinLockRelease(&g_accessTimingLock);return result;
}
}
