#pragma once
#include <stdint.h>
#include "tsc_clock_policy.h"

namespace brook {
// Invariant-TSC elapsed clock for hosts without a paravirtual clock (the
// physical Dell). Selected once on the BSP after SMP bring-up, while APs are
// still in their bootstrap wait, and never deselected. When it is selected,
// ApicTickCount(), CLOCK_MONOTONIC and CLOCK_REALTIME all derive from it; the
// LAPIC interrupt remains only the scheduling/timer-expiry tick.
//
// Logs exactly one `CLOCK_SOURCE ...` line describing the decision.
void TscClockSelect();
// Each AP, before scheduler entry: publish its CPU index in IA32_TSC_AUX so
// the vDSO selects a valid (identical) per-CPU record.
void TscClockInitOnCpu(uint32_t cpu);

bool TscClockAvailable();
// Nanoseconds in the ApicTickCount()*1e6 domain (continuous with the delivered
// tick count at selection). Never decreases across CPUs: a global floor hides
// any residual sub-bound offset. Returns 0 when unavailable.
uint64_t TscClockReadNs();

struct TscClockStatus {
    ClockSourceKind kind;const char* reason;
    bool decided,hypervisor,invariant,rdtscp,vdso,warpFree;
    uint32_t khz,cpus,syncCpus,syncFailedCpu;
    uint64_t syncMaxNs,syncMaxTicks,originTsc,originNs,floorHits;
    TscScale scale;
};
TscClockStatus TscClockReadStatus();

// The kernel's monotonic elapsed-ns source, in priority order: KVM pvclock,
// then invariant TSC, then delivered LAPIC ticks (ms resolution).
bool KvmClockAvailable();
uint64_t KvmClockReadNs();
uint64_t ApicTickCount();
inline uint64_t ElapsedClockNs(){
    if(KvmClockAvailable())return KvmClockReadNs();
    if(TscClockAvailable())return TscClockReadNs();
    return ApicTickCount()*1000000ULL;
}
inline bool ElapsedClockNsResolution(){return KvmClockAvailable() || TscClockAvailable();}
}
