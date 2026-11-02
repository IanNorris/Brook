#pragma once
#include "boot_protocol/boot_protocol.h"
#include "../../drivers/intel_hda/hda_probe_session.h"
namespace brook {
struct HdaObservation {
    bool requested=false, attempted=false, mapped=false;
    const char* reason="not requested";
    uint64_t base=0;
    HdaSessionReport session{};
};
// Called once on CPU 0 before AP scheduler activation or driver loading.
void HdaObserveConfigure(const BootProtocol* boot);
const HdaObservation& HdaObserveRead();
}
