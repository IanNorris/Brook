#pragma once
#include "compositor_handoff.h"
#include "intel_bdw_buffer.h"
#include "intel_bdw_fence.h"
#include "intel_bdw_exec_snapshot.h"

namespace brook {
// Intel resource adapter for the policy. Every operation is serialized by the
// caller's sleepable handoff mutex (never a spinlock). Final unrefs and fence
// inspection may allocate/free; the caller must obey the kernel lock order.
// Arm checks the exact buffer and signal fence against the retained submission
// before it becomes visible. The caller still excludes later writes and owns
// the snapshot's admission/release serialization.
// A retained buffer is not by itself proof of retirement, cache visibility,
// display ownership, or permission for userspace to reuse its contents.
struct BdwHandoffResources {
    CompositorHandoff policy;
    BdwGpuBuffer* buffers[BROOK_GPU_HANDOFF_MAX_DEPTH];
    BdwFence* fences[BROOK_GPU_HANDOFF_MAX_DEPTH];
    uint64_t tokens[BROOK_GPU_HANDOFF_MAX_DEPTH];
    uint64_t requests[BROOK_GPU_HANDOFF_MAX_DEPTH];
};
inline bool BdwHandoffReleaseFree(BdwHandoffResources& h,uint32_t i,const HandoffSlot& previous) {
    if(h.policy.slots[i].state!=HandoffSlotState::Free)return true;
    if(h.tokens[i] && !BdwGpuBufferReleaseHandoff(h.buffers[i],h.tokens[i])){
        // A state transition must never make a still-pinned/unsafe slot free.
        h.policy.slots[i]=previous;h.policy.slots[i].state=HandoffSlotState::Quarantined;return false;
    }
    h.tokens[i]=0;h.requests[i]=0;
    if(h.fences[i]){BdwFenceUnref(h.fences[i]);h.fences[i]=nullptr;}
    if(h.buffers[i]){BdwGpuBufferUnref(h.buffers[i]);h.buffers[i]=nullptr;}
    return true;
}
inline bool BdwHandoffReserve(BdwHandoffResources& h,uint64_t owner,uint64_t scene,uint64_t display,
                              BdwGpuBuffer* buffer,uint64_t gateToken,HandoffTicket& ticket) {
    if(!buffer || !gateToken || BdwGpuBufferAllowsUserMapping(buffer) || !HandoffReserve(h.policy,owner,scene,display,ticket))return false;
    BdwGpuBufferRef(buffer);h.buffers[ticket.slot]=buffer;
    if(!BdwGpuBufferReserveHandoff(buffer,gateToken)){
        HandoffCancelReservation(h.policy,owner,ticket);BdwGpuBufferUnref(buffer);h.buffers[ticket.slot]=nullptr;return false;
    }
    h.tokens[ticket.slot]=gateToken;return true;
}
// gateToken is kernel-generated and never reused for this buffer. If binding
// fails after reserving the gate, the caller discards the snapshot.
inline bool BdwHandoffArm(BdwHandoffResources& h,uint64_t owner,HandoffTicket ticket,BdwExecSnapshot* snapshot,BdwFence* fence,uint64_t gateToken) {
    auto* slot=HandoffFind(h.policy,ticket);
    if(!fence || !gateToken || !slot || h.policy.owner!=owner || slot->owner!=owner ||
       slot->state!=HandoffSlotState::Reserved || h.fences[ticket.slot] || h.tokens[ticket.slot]!=gateToken ||
       !BdwExecSnapshotWritesWithSignal(snapshot,h.buffers[ticket.slot],fence) ||
       !BdwGpuBufferHandoffMatches(h.buffers[ticket.slot],gateToken))return false;
    if(!BdwExecSnapshotBindHandoff(snapshot,h.buffers[ticket.slot],fence,gateToken))return false;
    if(!HandoffSubmit(h.policy,owner,ticket))return false;
    BdwFenceRef(fence);h.fences[ticket.slot]=fence;return true;
}
inline bool BdwHandoffCancel(BdwHandoffResources& h,uint64_t owner,HandoffTicket ticket) {
    auto* slot=HandoffFind(h.policy,ticket);if(!slot)return false;const HandoffSlot previous=*slot;
    if(!HandoffCancelReservation(h.policy,owner,ticket))return false;
    return BdwHandoffReleaseFree(h,ticket.slot,previous);
}
// Queue insertion has definitively failed before publishing the snapshot.
// The caller must discard the snapshot; its token is no longer usable.
inline bool BdwHandoffAbortUndelivered(BdwHandoffResources& h,uint64_t owner,HandoffTicket ticket,BdwExecSnapshot* snapshot) {
    auto* slot=HandoffFind(h.policy,ticket);
    if(!slot || h.policy.owner!=owner || slot->owner!=owner || slot->state!=HandoffSlotState::InFlight ||
       !BdwExecSnapshotHandoffUndelivered(snapshot,h.buffers[ticket.slot],h.tokens[ticket.slot]))return false;
    const HandoffSlot previous=*slot;*slot={};return BdwHandoffReleaseFree(h,ticket.slot,previous);
}
// Only the queue service may call this after it atomically proved the exact
// request never entered the engine, retired it, signaled failure, and released
// its snapshot. A generic failed fence alone is not such proof.
inline bool BdwHandoffRetireCancelled(BdwHandoffResources& h,uint64_t owner,HandoffTicket ticket){
    auto* slot=HandoffFind(h.policy,ticket);
    if(!slot || h.policy.owner!=owner || slot->owner!=owner || slot->state!=HandoffSlotState::InFlight ||
       !h.fences[ticket.slot] || BdwFenceState(h.fences[ticket.slot])!=2)return false;
    const HandoffSlot previous=*slot;*slot={};return BdwHandoffReleaseFree(h,ticket.slot,previous);
}
inline void BdwHandoffDetach(BdwHandoffResources& h,uint64_t owner) {
    HandoffSlot previous[BROOK_GPU_HANDOFF_MAX_DEPTH];
    for(uint32_t i=0;i<BROOK_GPU_HANDOFF_MAX_DEPTH;++i)previous[i]=h.policy.slots[i];
    HandoffDetach(h.policy,owner);
    for(uint32_t i=0;i<BROOK_GPU_HANDOFF_MAX_DEPTH;++i)BdwHandoffReleaseFree(h,i,previous[i]);
}
// Returns 0 pending/unknown, 1 successfully retired, 2 failed/quarantined.
inline uint32_t BdwHandoffObserve(BdwHandoffResources& h,HandoffTicket ticket) {
    auto* slot=HandoffFind(h.policy,ticket);
    if(!slot || (slot->state!=HandoffSlotState::InFlight && slot->state!=HandoffSlotState::Quarantined) || !h.fences[ticket.slot])return 0;
    const uint32_t state=BdwFenceState(h.fences[ticket.slot]);
    if(!state)return 0;
    const HandoffSlot previous=*slot;
    HandoffComplete(h.policy,ticket,state==1);
    if(state==1){
        if(h.policy.slots[ticket.slot].state==HandoffSlotState::Free){
            if(!BdwHandoffReleaseFree(h,ticket.slot,previous))return 2;
        } else {BdwFenceUnref(h.fences[ticket.slot]);h.fences[ticket.slot]=nullptr;}
    }
    return state;
}
inline bool BdwHandoffTake(BdwHandoffResources& h,HandoffTicket ticket,uint64_t scene,uint64_t display) {
    auto* slot=HandoffFind(h.policy,ticket);if(!slot)return false;const HandoffSlot previous=*slot;
    const bool ok=HandoffTake(h.policy,ticket,scene,display);
    BdwHandoffReleaseFree(h,ticket.slot,previous);
    return ok;
}
inline bool BdwHandoffFinish(BdwHandoffResources& h,HandoffTicket ticket) {
    auto* slot=HandoffFind(h.policy,ticket);if(!slot)return false;const HandoffSlot previous=*slot;
    if(!HandoffFinishPresentation(h.policy,ticket))return false;
    return BdwHandoffReleaseFree(h,ticket.slot,previous);
}
}
