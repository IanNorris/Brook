#pragma once
#include <stdint.h>
#include "../../shared/gpu_output_abi.h"

namespace brook {
// Policy only: the caller serializes all calls, owns and pins the actual
// buffers/fences, and publishes scanout only after taking a completed slot.
// Submit must precede any possible GPU access, and take must run under the
// lock that protects the authoritative scene/display generations.
// Owner is a non-reused process incarnation, never a bare PID.
enum class HandoffSlotState : uint8_t { Free, Reserved, InFlight, Ready, Presenting, Quarantined };
struct HandoffSlot {
    HandoffSlotState state;
    uint64_t owner, ticket, scene, display;
};
struct CompositorHandoff {
    uint64_t owner, nextTicket, lastPresented;
    HandoffSlot slots[BROOK_GPU_HANDOFF_MAX_DEPTH];
    uint32_t depth;
};
struct HandoffTicket { uint32_t slot; uint64_t ticket; };

inline bool HandoffAttach(CompositorHandoff& h,uint64_t owner,uint32_t depth=BROOK_GPU_HANDOFF_DEFAULT_DEPTH) {
    if(!owner || h.owner || depth<BROOK_GPU_HANDOFF_DEFAULT_DEPTH || depth>BROOK_GPU_HANDOFF_MAX_DEPTH)return false;
    h.owner=owner;h.lastPresented=0;h.depth=depth;return true;
}
// A later completed frame may be presented first, but never let an older
// completion replace it. Mark only after the OS accepted the presentation.
inline bool HandoffPresentationAllowed(const CompositorHandoff& h,uint64_t owner,HandoffTicket t){
    return owner && h.owner==owner && t.ticket && t.ticket>h.lastPresented;
}
inline void HandoffPresentationCommitted(CompositorHandoff& h,uint64_t owner,HandoffTicket t){
    if(HandoffPresentationAllowed(h,owner,t))h.lastPresented=t.ticket;
}
inline void HandoffDetach(CompositorHandoff& h,uint64_t owner) {
    if(!owner || h.owner!=owner)return;
    h.owner=0;
    for(auto& s:h.slots)if(s.owner==owner){
        if(s.state==HandoffSlotState::InFlight)s.state=HandoffSlotState::Quarantined;
        else if(s.state==HandoffSlotState::Reserved || s.state==HandoffSlotState::Ready)s={};
        // Presenting belongs to the OS until its copy/presentation is done.
    }
}
inline bool HandoffReserve(CompositorHandoff& h,uint64_t owner,uint64_t scene,uint64_t display,HandoffTicket& out) {
    if(!owner || h.owner!=owner || !scene || !display || h.nextTicket==~uint64_t{0})return false;
    for(uint32_t i=0;i<h.depth;++i)if(h.slots[i].state==HandoffSlotState::Free){
        const uint64_t ticket=++h.nextTicket;
        h.slots[i]={HandoffSlotState::Reserved,owner,ticket,scene,display};out={i,ticket};return true;
    }
    return false;
}
inline HandoffSlot* HandoffFind(CompositorHandoff& h,HandoffTicket t) {
    if(t.slot>=BROOK_GPU_HANDOFF_MAX_DEPTH || !t.ticket)return nullptr;
    auto& s=h.slots[t.slot];return s.ticket==t.ticket?&s:nullptr;
}
inline bool HandoffSubmit(CompositorHandoff& h,uint64_t owner,HandoffTicket t) {
    auto* s=HandoffFind(h,t);
    if(!s || h.owner!=owner || s->owner!=owner || s->state!=HandoffSlotState::Reserved)return false;
    s->state=HandoffSlotState::InFlight;return true;
}
inline bool HandoffCancelReservation(CompositorHandoff& h,uint64_t owner,HandoffTicket t) {
    auto* s=HandoffFind(h,t);
    if(!s || h.owner!=owner || s->owner!=owner || s->state!=HandoffSlotState::Reserved)return false;
    *s={};return true;
}
// A successful, independently verified completion retires GPU access. A
// failure or unknown completion is quarantined until a later proof of safety.
inline bool HandoffComplete(CompositorHandoff& h,HandoffTicket t,bool completed) {
    auto* s=HandoffFind(h,t);
    if(!s || (s->state!=HandoffSlotState::InFlight && s->state!=HandoffSlotState::Quarantined))return false;
    if(!completed){s->state=HandoffSlotState::Quarantined;return true;}
    if(s->state==HandoffSlotState::Quarantined || h.owner!=s->owner)*s={};
    else s->state=HandoffSlotState::Ready;
    return true;
}
// A stale frame is discarded, never presented. The OS holds a taken slot
// until HandoffFinishPresentation, independent of the renderer's lifetime.
inline bool HandoffTake(CompositorHandoff& h,HandoffTicket t,uint64_t scene,uint64_t display) {
    auto* s=HandoffFind(h,t);
    if(!s || s->state!=HandoffSlotState::Ready)return false;
    if(h.owner!=s->owner || s->scene!=scene || s->display!=display){*s={};return false;}
    s->state=HandoffSlotState::Presenting;return true;
}
inline bool HandoffFinishPresentation(CompositorHandoff& h,HandoffTicket t) {
    auto* s=HandoffFind(h,t);
    if(!s || s->state!=HandoffSlotState::Presenting)return false;
    *s={};return true;
}
}
