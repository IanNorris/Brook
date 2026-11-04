#pragma once
#include "intel_vtd_probe_policy.h"
namespace brook {
struct DellGpuVtdObservation {
    bool requested=false, topology=false, inventory=false, bootMap=false, sampled=false;
    bool bootMapClassified=false, bootMapUnreported=false, auditedDmarHole=false;
    uint32_t bootMapTypes=0;
    DellGpuVtdStatus status{};
};
// One BSP boot-time attempt; no device writes or DMA.
void DellGpuVtdObserveConfigure();
const DellGpuVtdObservation& DellGpuVtdObserveRead();
}
