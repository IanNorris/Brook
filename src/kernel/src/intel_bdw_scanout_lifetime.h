#pragma once
#include "compositor_scanout_lifetime.h"
#include "intel_bdw_handoff.h"

namespace brook {
// Unwired adapter: serialize with the existing sleepable handoff mutex and
// the future display owner. It transfers the protected output's retained
// reference and exclusive gate; it performs no GGTT or display MMIO access.
// The caller still proves format, DMA mapping, cache visibility, flip and
// display quiescence, as well as the absence of stale token-bearing producers.
struct BdwScanoutResources {
    ScanoutLifetime policy{};
    struct Entry { BdwGpuBuffer* buffer=nullptr;uint64_t token=0; } entries[2]{};
};
enum class BdwScanoutResult { Rejected, Completed, Retained };
inline bool BdwScanoutCollectRetired(BdwScanoutResources& s) {
    bool ok=true;
    for(auto& e:s.entries)if(e.buffer){
        const uint64_t id=BdwGpuBufferIdentity(e.buffer);
        if(id==s.policy.active || id==s.policy.pending)continue;
        if(!BdwGpuBufferReleaseHandoff(e.buffer,e.token)){ok=false;continue;}
        BdwGpuBufferUnref(e.buffer);e={};
    }
    return ok;
}
inline bool BdwScanoutAdopt(BdwScanoutResources& s,BdwHandoffResources& h,HandoffTicket t,uint64_t display) {
    auto* slot=HandoffFind(h.policy,t);
    if(!slot || slot->state!=HandoffSlotState::Presenting || slot->display!=display ||
       !h.buffers[t.slot] || h.fences[t.slot] || !h.tokens[t.slot] ||
       BdwGpuBufferAllowsUserMapping(h.buffers[t.slot]) ||
       !BdwGpuBufferHandoffCompleted(h.buffers[t.slot],h.tokens[t.slot]) || !BdwScanoutCollectRetired(s))return false;
    BdwScanoutResources::Entry* free=nullptr;
    for(auto& e:s.entries)if(!e.buffer){free=&e;break;}
    BdwGpuBuffer* buffer=h.buffers[t.slot];const uint64_t token=h.tokens[t.slot];
    if(!free || !ScanoutPrepare(s.policy,display,BdwGpuBufferIdentity(buffer),true))return false;
    *free={buffer,token};h.buffers[t.slot]=nullptr;h.tokens[t.slot]=0;h.requests[t.slot]=0;*slot={};
    return true;
}
// Release failures retain the reference and block new adoption until retried.
inline BdwScanoutResult BdwScanoutCancelUnissued(BdwScanoutResources& s) {
    if(!ScanoutCancelUnissued(s.policy))return BdwScanoutResult::Rejected;
    return BdwScanoutCollectRetired(s)?BdwScanoutResult::Completed:BdwScanoutResult::Retained;
}
inline BdwScanoutResult BdwScanoutComplete(BdwScanoutResources& s,uint64_t display,uint64_t live) {
    uint64_t retired=0;if(!ScanoutComplete(s.policy,display,live,retired))return BdwScanoutResult::Rejected;
    return BdwScanoutCollectRetired(s)?BdwScanoutResult::Completed:BdwScanoutResult::Retained;
}
inline BdwScanoutResult BdwScanoutQuiesced(BdwScanoutResources& s) {
    uint64_t active=0,pending=0;if(!ScanoutQuiesced(s.policy,active,pending))return BdwScanoutResult::Rejected;
    return BdwScanoutCollectRetired(s)?BdwScanoutResult::Completed:BdwScanoutResult::Retained;
}
}
