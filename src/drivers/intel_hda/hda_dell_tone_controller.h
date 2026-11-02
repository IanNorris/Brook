#pragma once
#include "hda_probe_session.h"
namespace brook {
// Position-buffer DMA is independent of CORB/RIRB and stream RUN. This
// LPIB-only diagnostic must not allow the controller to write through a
// firmware-owned position-buffer address; it never changes that address.
template<class Io> bool HdaDellToneControllerIdle(Io& io,const HdaSessionReport& snapshot) {
    const uint32_t positionLow=io.read32(0x70);(void)io.read32(0x74);
    if(positionLow&1)return false;
    HdaProbeController state{};
    return HdaProbeInspect(io,state)==HdaProbeResult::Ready &&
           state.gcap==snapshot.controller.gcap && (state.state&1);
}
}
