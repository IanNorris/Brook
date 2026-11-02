#pragma once
#include <stdint.h>
#include "native_tsc_rendezvous.h"
namespace brook {
struct NativeTscObservation {
    bool requested=false,attempted=false,capable=false,measured=false,consistent=false;
    uint32_t khz=0,pitTicks=0;uint64_t samples[5]{};
    uint32_t coreExpected=0,coreCount=0;NativeTscCpuObservation cores[63]{};
    const char* reason="not requested";
};
// One-shot BSP-only diagnostic. It never publishes a clock or changes the
// scheduler timer; an explicit initrd marker is required.
void NativeTscObserveConfigure();
const NativeTscObservation& NativeTscObserveRead();
}
