#pragma once
#include "intel_bdw_gtt_probe.h"
namespace brook {
constexpr uint64_t DellForcewakeAckPage=0xf6130000ull;
// Read-only snapshot of the always-on ACK. This cannot establish ownership.
// Caller verifies Dell identity, D0, boot-map/PCI resources and explicit opt-in.
template<class Io> bool BdwForcewakeAckCritical(Io& io,uint32_t& out) {
    DellI2cCriticalEvidence e{};
    if(!io.evidence(e) || !BdwGttEvidenceValid(e))return false;
    uint32_t v=0;if(!io.read(v) || v==0xffffffffu)return false;
    out=v;return true;
}
}
