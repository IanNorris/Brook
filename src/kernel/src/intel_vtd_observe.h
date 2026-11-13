#pragma once
#include "intel_vtd_probe_policy.h"
namespace brook {
struct DellGpuVtdObservation {
    bool deferred=false, deferredAttempted=false;
    bool requested=false, topology=false, inventory=false, bootMap=false, sampled=false;
    bool bootMapClassified=false, bootMapUnreported=false, auditedDmarHole=false;
    uint32_t bootMapTypes=0;
    DellGpuVtdStatus status{};
};
// One BSP boot-time attempt; no device writes or DMA.
void DellGpuVtdObserveConfigure();
// Deferred sampling is accepted only when recovery reset is already armed.
// It remains read-only and does not grant GPU DMA or ownership.
bool DellGpuVtdObserveDeferred();
const DellGpuVtdObservation& DellGpuVtdObserveRead();
}
