#pragma once
#include "intel_bdw_refusal.h"
#include "intel_bdw_exec_queue.h"
#include "intel_bdw_handoff.h"
#include "intel_bdw_preparation.h"
#include "intel_bdw_submission_timing.h"
#include "sync/kmutex.h"

namespace brook {
// Nodes live on the reaper's stack and are linked only while holding the
// submission mutex. Count the interval after queue removal and before the
// snapshot's retained references have actually been released.
class BdwRetirementLedger {
public:
    struct Node {uint64_t owner=0;Node* next=nullptr;};
private:
    Node* head_=nullptr;
public:
    void Add(Node& node,uint64_t owner){node.owner=owner;node.next=head_;head_=&node;}
    bool Remove(Node& node){Node** p=&head_;while(*p && *p!=&node)p=&(*p)->next;if(!*p)return false;*p=node.next;node.next=nullptr;return true;}
    void Count(uint64_t owner,BdwExecOwnerWork& out)const{
        for(auto* p=head_;p;p=p->next)if(!p->owner)++out.unknown;else if(p->owner==owner)++out.retiring;
    }
};
// Queue-space waiting (execbuffer back-pressure). Linux i915 blocks an
// execbuffer until it can be queued and Mesa treats any other failure as
// fatal, so a full queue must make the caller wait, not fail. A caller that
// can sleep reserves its slot before decoding (ReservePreparation) with no
// locks held; the slot is consumed by exactly the snapshot it later
// publishes in its entry, so admission (ordinary or handoff) cannot then
// fail for capacity. Clients are capped so desktop presentation keeps a
// reserved slice: with a present owner (the attached handoff renderer),
// other owners admit only while fewer than Capacity-PresentReserve slots are
// used, and any one non-present owner holds at most ClientLimit.
// Serialized service-side ownership for one permanent engine worker.
// Init must complete before publication. Submit transfers the snapshot only
// on success; Take transfers no ownership, and the worker must call Finish
// after device/cache cleanup. This class does not start a thread, authorize
// commands or device DMA, or recover a quarantined engine.
class BdwSubmissionQueue {
    KMutex mutex_{};BdwExecQueue queue_;BdwRetirementLedger retiring_;BdwSubmissionPreparation* preparing_=nullptr;
    bool initialized_=false,retirementFailed_=false,knownOwnersOnly_=false;uint64_t frozenOwner_=0;
    uint32_t reserved_=0,waiters_=0;BdwQueueSpaceStats space_{}; // under mutex_
    unsigned ReservedByLocked(uint64_t owner)const{
        unsigned n=0;for(auto* p=preparing_;p;p=p->next)if(p->reserved && p->owner==owner)++n;return n;
    }
    bool SpaceForLocked(uint64_t owner,uint64_t presentOwner)const{
        const bool present=presentOwner && owner==presentOwner;
        const unsigned limit=presentOwner && !present?BdwExecQueue::Capacity-PresentReserve:BdwExecQueue::Capacity;
        if(queue_.Occupied()+reserved_>=limit)return false;
        return present || queue_.Occupied(owner)+ReservedByLocked(owner)<ClientLimit;
    }
    // The reservation this snapshot's own preparer published, if any.
    BdwSubmissionPreparation* ReservationLocked(const BdwExecSnapshot* snapshot)const{
        for(auto* p=preparing_;p;p=p->next)if(p->reserved && __atomic_load_n(&p->snapshot,__ATOMIC_ACQUIRE)==snapshot)return p;
        return nullptr;
    }
    // Unreserved callers may not take a slot promised to a reservation.
    bool RoomLocked(const BdwSubmissionPreparation* reservation)const{
        return reservation || queue_.Occupied()+reserved_<BdwExecQueue::Capacity;
    }
    void ConsumeLocked(BdwSubmissionPreparation* reservation){if(reservation){reservation->reserved=false;--reserved_;}}
    // Called under mutex_ after slots were freed (or the queue poisoned).
    // Costs one load when nobody waits; the signal itself is issued unlocked.
    bool SpaceWakeLocked(bool freed){const bool wake=freed && waiters_;if(wake)++space_.signals;return wake;}
    static void SignalSpace(){BdwGpuEventSignal(BdwGpuEvent::Space);}
    bool InspectOwnerLocked(uint64_t owner,BdwExecOwnerWork& out){
        if(retirementFailed_ || !queue_.InspectOwner(owner,out))return false;
        retiring_.Count(owner,out);
        for(auto* p=preparing_;p;p=p->next)if(!p->owner)++out.unknown;else if(p->owner==owner)++out.preparing;
        return true;
    }
    bool ReleaseRetired(BdwExecSnapshot* snapshot,BdwRetirementLedger::Node& node){
        const bool released=BdwExecSnapshotRelease(snapshot,false);
        KMutexLock(&mutex_);if(!released)retirementFailed_=true;
        if(!retiring_.Remove(node))__builtin_trap();KMutexUnlock(&mutex_);return released;
    }
    bool AdmissionBlocked(const BdwExecSnapshot* snapshot)const{
        const auto owner=BdwExecSnapshotOwner(snapshot);
        return frozenOwner_ && (!owner || owner==frozenOwner_);
    }
public:
    static constexpr unsigned PresentReserve=8,ClientLimit=16;
    static_assert(ClientLimit<=BdwExecQueue::Capacity-PresentReserve,"client cap within the shared slice");
    BdwSubmissionQueue()=default;
    BdwSubmissionQueue(const BdwSubmissionQueue&)=delete;
    BdwSubmissionQueue& operator=(const BdwSubmissionQueue&)=delete;
    bool Init(bool knownOwnersOnly=false){if(initialized_)return false;KMutexInit(&mutex_);knownOwnersOnly_=knownOwnersOnly;initialized_=true;return true;}
    int64_t BeginPreparation(uint64_t owner,BdwSubmissionPreparation& entry){
        if(!initialized_)return -19;if(knownOwnersOnly_ && !owner)return -22;KMutexLock(&mutex_);
        if(frozenOwner_ && (!owner || owner==frozenOwner_)){KMutexUnlock(&mutex_);BdwNoteRefusal("prepare: owner admission frozen (blocked recovery)");return -16;}
        entry.owner=owner;entry.next=preparing_;preparing_=&entry;KMutexUnlock(&mutex_);return 0;
    }
    // Reserve one slot for an exec that will publish its snapshot in entry.
    // Process context with no locks held. 0: entry is registered (as by
    // BeginPreparation) and holds a slot until consumed or ended. -11: no
    // slot yet; entry is unregistered but counted as a waiter. The caller
    // samples BdwGpuEvent::Space before each call, sleeps on it after -11,
    // then calls again or abandons with EndPreparation. Poisoned (-5) and
    // frozen-owner (-16) refusals are immediate and end the wait.
    int64_t ReservePreparation(uint64_t owner,uint64_t presentOwner,BdwSubmissionPreparation& entry){
        if(!initialized_)return -19;if(knownOwnersOnly_ && !owner)return -22;KMutexLock(&mutex_);
        const bool poisoned=queue_.Poisoned();
        const char* refusal=poisoned?"prepare: queue poisoned":
            frozenOwner_ && (!owner || owner==frozenOwner_)?"prepare: owner admission frozen (blocked recovery)":nullptr;
        if(!refusal && !SpaceForLocked(owner,presentOwner)){
            if(!entry.waiting){entry.waiting=true;++waiters_;++space_.waits;}
            KMutexUnlock(&mutex_);return -11;
        }
        if(entry.waiting){entry.waiting=false;--waiters_;if(refusal)++space_.refused;else ++space_.granted;}
        if(refusal){KMutexUnlock(&mutex_);BdwNoteRefusal(refusal);return poisoned?-5:-16;}
        entry.owner=owner;entry.snapshot=nullptr;entry.reserved=true;++reserved_;
        entry.next=preparing_;preparing_=&entry;KMutexUnlock(&mutex_);return 0;
    }
    // Ends a registered entry (releasing an unconsumed reservation) or an
    // unfinished ReservePreparation wait.
    void EndPreparation(BdwSubmissionPreparation& entry){
        KMutexLock(&mutex_);
        if(entry.waiting){entry.waiting=false;--waiters_;++space_.abandoned;KMutexUnlock(&mutex_);return;}
        auto** p=&preparing_;while(*p && *p!=&entry)p=&(*p)->next;
        if(!*p)__builtin_trap();*p=entry.next;entry.next=nullptr;
        const bool released=entry.reserved;if(released){entry.reserved=false;--reserved_;}
        __atomic_store_n(&entry.snapshot,static_cast<BdwExecSnapshot*>(nullptr),__ATOMIC_RELEASE);
        const bool wake=SpaceWakeLocked(released);KMutexUnlock(&mutex_);
        if(wake)SignalSpace();
    }
    BdwQueueSpaceStats SpaceStats(){
        BdwQueueSpaceStats out{};if(!initialized_)return out;KMutexLock(&mutex_);
        out=space_;out.occupied=queue_.Occupied();out.reserved=reserved_;out.waiters=waiters_;KMutexUnlock(&mutex_);return out;
    }
    bool Poisoned(){if(!initialized_)return true;KMutexLock(&mutex_);const bool poisoned=queue_.Poisoned();KMutexUnlock(&mutex_);return poisoned;}
    // An in-progress admission has not exposed its buffer list yet. Refuse a
    // snapshot until every such transaction has either queued or failed.
    BdwFence* AcquireBufferFence(BdwGpuBuffer* buffer){
        if(!initialized_ || !buffer)return nullptr;
        KMutexLock(&mutex_);
        auto* result=(!preparing_ && !retirementFailed_)?queue_.AcquireBufferFence(buffer):nullptr;
        KMutexUnlock(&mutex_);return result;
    }
    bool InspectOwner(uint64_t owner,BdwExecOwnerWork& out){
        if(!initialized_)return false;KMutexLock(&mutex_);const bool ok=InspectOwnerLocked(owner,out);KMutexUnlock(&mutex_);return ok;
    }
    // Freeze and observe under the same queue lock as admission. Unknown-owner
    // submissions are refused while frozen because they may belong to it.
    // This is not a proof that shared buffers or other GPU paths are idle.
    bool FreezeOwner(uint64_t owner,BdwExecOwnerWork& out){
        if(!initialized_ || !owner)return false;KMutexLock(&mutex_);
        const bool ok=(!frozenOwner_ || frozenOwner_==owner) && InspectOwnerLocked(owner,out);
        if(ok)frozenOwner_=owner;KMutexUnlock(&mutex_);return ok;
    }
    bool ThawOwner(uint64_t owner){
        if(!initialized_ || !owner)return false;KMutexLock(&mutex_);
        const bool ok=frozenOwner_==owner;if(ok)frozenOwner_=0;KMutexUnlock(&mutex_);return ok;
    }
    bool IsFrozenOwner(uint64_t owner){
        if(!initialized_ || !owner)return false;KMutexLock(&mutex_);
        const bool result=frozenOwner_==owner;KMutexUnlock(&mutex_);return result;
    }
    bool FrozenOwnerWork(uint64_t owner,BdwExecOwnerWork& out){
        if(!initialized_ || !owner)return false;KMutexLock(&mutex_);
        const bool ok=frozenOwner_==owner && InspectOwnerLocked(owner,out);KMutexUnlock(&mutex_);return ok;
    }
    // All calls below require process context and an initialized service.
    int64_t Submit(BdwExecSnapshot* snapshot){
        if(!initialized_ || !snapshot || (knownOwnersOnly_ && !BdwExecSnapshotOwner(snapshot)))return -22;
        KMutexLock(&mutex_);uint64_t id=0;auto* reservation=ReservationLocked(snapshot);
        const bool poisoned=queue_.Poisoned(),blocked=AdmissionBlocked(snapshot),room=RoomLocked(reservation);
        const bool accepted=!poisoned && !blocked && room && queue_.Enqueue(1,snapshot,id);
        if(accepted)ConsumeLocked(reservation);
#if defined(BROOK_GPU_SUBMISSION_TIMING)
        if(accepted)BdwTimingQueued(snapshot);
#endif
        if(!accepted){const char* use=room?BdwExecSnapshotUseRefusal(snapshot):nullptr;
            BdwNoteRefusal(poisoned?"submit: queue poisoned":blocked?"submit: owner admission frozen":use?use:"submit: exec queue full (32 entries)");}
        KMutexUnlock(&mutex_);return accepted?0:poisoned?-5:-16;
    }
    // Caller holds the sleepable handoff mutex before entering the queue lock.
    // On rejection the snapshot is still caller-owned and must be discarded.
    int64_t SubmitHandoff(BdwHandoffResources& handoff,uint64_t owner,HandoffTicket ticket,
                          BdwExecSnapshot* snapshot,BdwFence* fence,uint64_t gateToken){
        if(!initialized_ || !snapshot || (knownOwnersOnly_ && (!owner || BdwExecSnapshotOwner(snapshot)!=owner)))return -22;
        KMutexLock(&mutex_);uint64_t id=0;auto* reservation=ReservationLocked(snapshot);
        if(queue_.Poisoned()){KMutexUnlock(&mutex_);BdwNoteRefusal("handoff submit: queue poisoned");return -5;}
        if((frozenOwner_ && owner==frozenOwner_) || AdmissionBlocked(snapshot)){KMutexUnlock(&mutex_);BdwNoteRefusal("handoff submit: owner admission frozen");return -16;}
        // Refuse a full queue before arming, so no ticket is armed and
        // aborted. A reserved caller always has its slot.
        if(!RoomLocked(reservation)){KMutexUnlock(&mutex_);BdwNoteRefusal("handoff submit: exec queue full (32 entries)");return -16;}
        if(!BdwHandoffArm(handoff,owner,ticket,snapshot,fence,gateToken)){KMutexUnlock(&mutex_);
            BdwNoteRefusal("handoff submit: arm refused (output not written with the signal fence, or its gate token changed)");return -22;}
        const bool accepted=queue_.Enqueue(1,snapshot,id);
        if(accepted)ConsumeLocked(reservation);
#if defined(BROOK_GPU_SUBMISSION_TIMING)
        if(accepted)BdwTimingQueued(snapshot);
#endif
        if(!accepted && !BdwHandoffAbortUndelivered(handoff,owner,ticket,snapshot)){
            KMutexUnlock(&mutex_);BdwNoteRefusal("handoff submit: undelivered abort failed (quarantined)");return -5; // retained/quarantined; never retry
        }
        if(accepted)handoff.requests[ticket.slot]=id;
        else{const char* use=BdwExecSnapshotUseRefusal(snapshot);BdwNoteRefusal(use?use:"handoff submit: enqueue refused");}
        KMutexUnlock(&mutex_);return accepted?0:-16;
    }
    // The handoff owner holds its mutex. The queue lock makes the queued vs
    // active decision atomic with worker admission; no active work is freed.
    int64_t CancelBlockedHandoff(BdwHandoffResources& handoff,uint64_t owner,HandoffTicket ticket){
        if(!initialized_ || ticket.slot>=BROOK_GPU_HANDOFF_MAX_DEPTH || !owner)return -22;
        auto* slot=HandoffFind(handoff.policy,ticket);
        if(!slot || handoff.policy.owner!=owner || slot->owner!=owner || slot->state!=HandoffSlotState::InFlight)return -2;
        const uint64_t id=handoff.requests[ticket.slot];if(!id)return -2;
        KMutexLock(&mutex_);
        const bool cancelled=queue_.CancelBlocked(1,id);
        BdwExecRequestState state{};
        auto* snapshot=cancelled?queue_.Reap(1,id,state):nullptr;BdwRetirementLedger::Node retirement{};
        if(snapshot)retiring_.Add(retirement,BdwExecSnapshotOwner(snapshot));
        if(cancelled && (!snapshot || state!=BdwExecRequestState::Cancelled))retirementFailed_=true;
        const bool wake=SpaceWakeLocked(snapshot!=nullptr);KMutexUnlock(&mutex_);if(wake)SignalSpace();
        if(!cancelled)return -11;
        if(!snapshot || state!=BdwExecRequestState::Cancelled){if(snapshot)ReleaseRetired(snapshot,retirement);return -5;}
        if(!ReleaseRetired(snapshot,retirement))return -5;
        if(!BdwHandoffRetireCancelled(handoff,owner,ticket))return -5;
        return 0;
    }
    // Atomically freeze admission and cancel only a matching never-admitted
    // request. A successful freeze persists until explicitly thawed.
    int64_t FreezeAndCancelBlockedHandoff(BdwHandoffResources& handoff,uint64_t owner,HandoffTicket ticket){
        if(!initialized_ || ticket.slot>=BROOK_GPU_HANDOFF_MAX_DEPTH || !owner)return -22;
        auto* slot=HandoffFind(handoff.policy,ticket);
        if(!slot || handoff.policy.owner!=owner || slot->owner!=owner || slot->state!=HandoffSlotState::InFlight)return -2;
        const uint64_t id=handoff.requests[ticket.slot];if(!id)return -2;
        KMutexLock(&mutex_);
        if(frozenOwner_){KMutexUnlock(&mutex_);return -16;}
        const bool cancelled=queue_.CancelBlockedForOrigin(1,id,owner);
        BdwExecRequestState state{};
        auto* snapshot=cancelled?queue_.Reap(1,id,state):nullptr;BdwRetirementLedger::Node retirement{};
        if(snapshot)retiring_.Add(retirement,BdwExecSnapshotOwner(snapshot));
        if(cancelled && (!snapshot || state!=BdwExecRequestState::Cancelled))retirementFailed_=true;
        if(cancelled)frozenOwner_=owner;
        const bool wake=SpaceWakeLocked(snapshot!=nullptr);KMutexUnlock(&mutex_);if(wake)SignalSpace();
        if(!cancelled)return -11;
        if(!snapshot || state!=BdwExecRequestState::Cancelled){if(snapshot)ReleaseRetired(snapshot,retirement);return -5;}
        if(!ReleaseRetired(snapshot,retirement))return -5;
        if(!BdwHandoffRetireCancelled(handoff,owner,ticket))return -5;
        return 0;
    }
    // No new same-owner/unknown work can enter while frozen. Cancel queued
    // work under the admission lock; active/quarantined work is never reaped.
    int64_t CancelFrozenOwnerQueued(BdwHandoffResources& handoff,uint64_t owner){
        if(!initialized_ || !owner)return -22;
        BdwExecSnapshot* finished[32]{};BdwRetirementLedger::Node retirements[32]{};uint64_t ids[32]{};uint32_t cancelled=0;
        KMutexLock(&mutex_);
        if(frozenOwner_!=owner){KMutexUnlock(&mutex_);return -2;}
        const bool ok=queue_.CancelQueuedForOrigin(1,owner,ids,cancelled);
        for(unsigned i=0;i<cancelled;++i){BdwExecRequestState state{};
            finished[i]=queue_.Reap(1,ids[i],state);
            if(!finished[i] || state!=BdwExecRequestState::Cancelled)retirementFailed_=true;
            if(finished[i])retiring_.Add(retirements[i],BdwExecSnapshotOwner(finished[i]));}
        const bool wake=SpaceWakeLocked(cancelled!=0);KMutexUnlock(&mutex_);if(wake)SignalSpace();
        bool released=true;for(unsigned i=0;i<cancelled;++i){
            if(!finished[i] || !ReleaseRetired(finished[i],retirements[i])){released=false;continue;}
            for(uint32_t slot=0;slot<BROOK_GPU_HANDOFF_MAX_DEPTH;++slot)if(handoff.requests[slot]==ids[i]){
                const HandoffTicket ticket{slot,handoff.policy.slots[slot].ticket};
                if(!BdwHandoffRetireCancelled(handoff,owner,ticket))released=false;
            }
        }
        return ok && released?static_cast<int64_t>(cancelled):-5;
    }
    bool Park(uint64_t id){
        if(!initialized_)return false;
        KMutexLock(&mutex_);const bool ok=queue_.Park(id);KMutexUnlock(&mutex_);return ok;
    }
    bool Take(uint64_t& id,BdwExecSnapshot*& snapshot,bool applicationOnly=false){
        if(!initialized_)return false;
        KMutexLock(&mutex_);const bool ok=queue_.StartNext(id,snapshot,applicationOnly);
#if defined(BROOK_GPU_SUBMISSION_TIMING)
        if(ok)BdwTimingTaken(snapshot);
#endif
        KMutexUnlock(&mutex_);return ok;
    }
    // Return false for any invariant failure or unsafe stop; the caller must
    // initiate recovery. Safely completed/aborted records are released
    // outside the mutex. A quarantined active snapshot stays owned forever.
    bool Finish(uint64_t id,bool success,bool safelyStopped,bool skipped=false){
        if(!initialized_)return false;
        bool cacheReady=false;
        if(success && safelyStopped){
            KMutexLock(&mutex_);auto* snapshot=queue_.ActiveSnapshot(id);KMutexUnlock(&mutex_);
            if(!snapshot)return false;
            // The engine owner is the only finisher. Active records cannot be
            // canceled or reaped, so their buffers stay reserved and pinned.
            cacheReady=BdwExecSnapshotSyncForRetirement(snapshot);
        }
        BdwExecSnapshot* finished[32]{};BdwRetirementLedger::Node retirements[32]{};unsigned count=0;
        KMutexLock(&mutex_);
        const bool ok=success && safelyStopped?
            queue_.FinishWithCacheResult(id,true,true,cacheReady):queue_.Finish(id,success,safelyStopped,skipped);
        const bool poisoned=queue_.Poisoned();
        if(ok)while(count<32){auto* item=queue_.ReapAnyFinished();if(!item)break;finished[count]=item;retiring_.Add(retirements[count],BdwExecSnapshotOwner(item));++count;}
        // Waiters re-check after a freed slot, or to fail on a poisoned queue.
        const bool wake=SpaceWakeLocked(count || poisoned);
        KMutexUnlock(&mutex_);
        for(unsigned i=0;i<count;++i)if(!ReleaseRetired(finished[i],retirements[i]))__builtin_trap();
        if(wake)SignalSpace();
        return ok && safelyStopped && !poisoned;
    }
};
}
