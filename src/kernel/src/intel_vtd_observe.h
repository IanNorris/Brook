#pragma once
#include "intel_vtd_probe_policy.h"
namespace brook {
struct DellGpuVtdObservation {
    bool requested=false, topology=false, inventory=false, bootMap=false, sampled=false;
    DellGpuVtdStatus status{};
};
// One BSP boot-time attempt; no device writes or DMA.
void DellGpuVtdObserveConfigure();
const DellGpuVtdObservation& DellGpuVtdObserveRead();
}
