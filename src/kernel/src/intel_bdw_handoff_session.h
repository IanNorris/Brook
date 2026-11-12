#pragma once
#include "intel_bdw_submission_worker.h"

namespace brook {
// Caller serializes every method with one sleepable mutex, including Submit
// and process-exit cleanup. Generation validation belongs to the presenter.
class BdwHandoffSession {
    BdwHandoffResources resources_{};
    uint64_t nextToken_=0,recoveryOwner_=0;
#ifdef BROOK_GPU_BLOCKED_RECOVERY_TEST
    uint64_t testBlockedOwner_=0,testBlockedTicket_=0;
#endif
    HandoffTicket Find(uint64_t value) const {
        for(uint32_t i=0;i<BROOK_GPU_HANDOFF_MAX_DEPTH;++i)
            if(value && resources_.policy.slots[i].ticket==value)return {i,value};
        return {BROOK_GPU_HANDOFF_MAX_DEPTH,0};
    }
    void Reap() {
        for(uint32_t i=0;i<BROOK_GPU_HANDOFF_MAX_DEPTH;++i){
            auto& s=resources_.policy.slots[i];
            if(s.state==HandoffSlotState::Quarantined && resources_.fences[i])
                BdwHandoffObserve(resources_,{i,s.ticket});
        }
    }
public:
    bool Attach(uint64_t owner,uint32_t depth=BROOK_GPU_HANDOFF_DEFAULT_DEPTH){
        Reap();return HandoffAttach(resources_.policy,owner,depth);
    }
    bool Detach(uint64_t owner){if(recoveryOwner_==owner && owner)return false;
#ifdef BROOK_GPU_BLOCKED_RECOVERY_TEST
        if(testBlockedOwner_==owner && owner)testBlockedOwner_=testBlockedTicket_=0;
#endif
        BdwHandoffDetach(resources_,owner);Reap();return true;}
    bool OwnedBy(uint64_t owner) const{return owner && resources_.policy.owner==owner;}
    bool ContainsTicket(uint64_t owner,uint64_t ticket) const {
        auto t=Find(ticket);return t.ticket && resources_.policy.slots[t.slot].owner==owner &&
            resources_.policy.slots[t.slot].state!=HandoffSlotState::Free;
    }
#ifdef BROOK_GPU_BLOCKED_RECOVERY_TEST
    bool TestBlockNext(uint64_t owner,uint64_t value){
        auto* slot=HandoffFind(resources_.policy,Find(value));
        if(testBlockedOwner_ || !slot || resources_.policy.owner!=owner || slot->owner!=owner || slot->state!=HandoffSlotState::Reserved)return false;
        testBlockedOwner_=owner;testBlockedTicket_=value;return true;
    }
#endif
    bool Idle(){Reap();for(const auto& slot:resources_.policy.slots)if(slot.state!=HandoffSlotState::Free)return false;return true;}
    int64_t Reserve(uint64_t owner,BdwGpuBuffer* buffer,uint64_t scene=1,uint64_t display=1){
        Reap();
        // Syscall returns a positive signed ticket; neither tickets nor gate
        // tokens may wrap and accidentally authorize a stale request.
        if(resources_.policy.owner!=owner || !owner)return -1;
        if(resources_.policy.nextTicket>=0x7fffffffffffffffull || nextToken_==~uint64_t{0})return -75;
        HandoffTicket ticket{};const uint64_t token=++nextToken_;
        if(!BdwHandoffReserve(resources_,owner,scene,display,buffer,token,ticket))return -16;
        return static_cast<int64_t>(ticket.ticket);
    }
    // 0 pending, 1 completed, 2 failure; negative values are errors.
    int64_t Observe(uint64_t owner,uint64_t value){
        const auto ticket=Find(value);auto* slot=HandoffFind(resources_.policy,ticket);
        if(!slot || resources_.policy.owner!=owner || slot->owner!=owner)return -2;
        if(slot->state==HandoffSlotState::Ready)return 1;
        if(slot->state==HandoffSlotState::Quarantined)return 2;
        if(slot->state==HandoffSlotState::Reserved)return 0;
        if(slot->state!=HandoffSlotState::InFlight)return -22;
        return BdwHandoffObserve(resources_,ticket);
    }
    int64_t Release(uint64_t owner,uint64_t value){
        const auto ticket=Find(value);auto* slot=HandoffFind(resources_.policy,ticket);
        if(!slot || resources_.policy.owner!=owner || slot->owner!=owner)return -2;
        if(slot->state==HandoffSlotState::Reserved)return BdwHandoffCancel(resources_,owner,ticket)?0:-5;
        if(slot->state!=HandoffSlotState::Ready)return -16;
        return BdwHandoffTake(resources_,ticket,slot->scene,slot->display) && BdwHandoffFinish(resources_,ticket)?0:-5;
    }
    int64_t CancelBlocked(uint64_t owner,uint64_t value,BdwSubmissionWorker& worker){
        return worker.CancelBlockedHandoff(resources_,owner,Find(value));
    }
    // Callers serialize with the handoff mutex. A successful freeze remains
    // owned by this session until explicitly abandoned or the owner exits.
    int64_t BeginBlockedRecovery(uint64_t owner,uint64_t value,BdwSubmissionWorker& worker){
        if(recoveryOwner_)return -16;
        const auto result=worker.FreezeAndCancelBlockedHandoff(resources_,owner,Find(value));
        // A failed post-cancellation release can leave the queue frozen.
        if((result==0 || result==-5) && worker.IsFrozenOwner(owner))recoveryOwner_=owner;
        if(result)return result;
        return worker.CancelFrozenOwnerQueued(resources_,owner)<0?-5:0;
    }
    bool RecoveryWork(uint64_t owner,BdwSubmissionWorker& worker,BdwExecOwnerWork& out){
        return owner && recoveryOwner_==owner && worker.FrozenOwnerWork(owner,out);
    }
    bool AbandonRecovery(uint64_t owner,BdwSubmissionWorker& worker){
        if(!owner || recoveryOwner_!=owner || !worker.ThawOwner(owner))return false;
        recoveryOwner_=0;return true;
    }
    void OwnerExit(uint64_t owner,BdwSubmissionWorker& worker){
        if(!owner)return;
#ifdef BROOK_GPU_BLOCKED_RECOVERY_TEST
        if(testBlockedOwner_==owner)testBlockedOwner_=testBlockedTicket_=0;
#endif
        BdwHandoffDetach(resources_,owner);Reap();
        if(recoveryOwner_==owner && worker.ThawOwner(owner))recoveryOwner_=0;
    }
    // Retain exclusive GPU ownership until the presenter has finished.
    template<class Present> int64_t PresentProbe(uint64_t owner,uint64_t value,Present present){
        const auto ticket=Find(value);auto* slot=HandoffFind(resources_.policy,ticket);
        if(!slot || resources_.policy.owner!=owner || slot->owner!=owner)return -2;
        if(slot->state!=HandoffSlotState::Ready)return -16;
        if(!HandoffPresentationAllowed(resources_.policy,owner,ticket))
            return Release(owner,value)?-5:-116; // retire stale output without presenting
        const auto scene=slot->scene,display=slot->display;
        if(!BdwHandoffTake(resources_,ticket,scene,display))return -5;
        const int64_t result=present(resources_.buffers[ticket.slot],resources_.tokens[ticket.slot],scene,display);
        if(result>=0)HandoffPresentationCommitted(resources_.policy,owner,ticket);
        return BdwHandoffFinish(resources_,ticket)?result:-5;
    }
    // Claim every reference to any retained output, even from the wrong owner
    // or in the wrong state, so it cannot fall through to ordinary submission.
    bool Submit(uint64_t owner,BdwSubmissionWorker& worker,BdwExecSnapshot* snapshot,int64_t& result){
        HandoffTicket ticket{BROOK_GPU_HANDOFF_MAX_DEPTH,0};
        for(uint32_t i=0;i<BROOK_GPU_HANDOFF_MAX_DEPTH;++i)if(resources_.buffers[i] && BdwExecSnapshotContainsBuffer(snapshot,resources_.buffers[i])){
            if(ticket.ticket){result=-16;return true;}
            ticket={i,resources_.policy.slots[i].ticket};
        }
        if(!ticket.ticket)return false;
        auto* slot=HandoffFind(resources_.policy,ticket);
        if(!slot || !owner || resources_.policy.owner!=owner || slot->owner!=owner || slot->state!=HandoffSlotState::Reserved){result=-16;return true;}
        auto* fence=BdwExecSnapshotAcquireWriteSignal(snapshot,resources_.buffers[ticket.slot]);
        if(!fence){result=-22;return true;}
#ifdef BROOK_GPU_BLOCKED_RECOVERY_TEST
        if(testBlockedOwner_==owner && testBlockedTicket_==ticket.ticket){
            testBlockedOwner_=testBlockedTicket_=0;auto* dependency=BdwFenceCreate();
            if(!dependency || !BdwExecSnapshotAddWaitFence(snapshot,dependency)){
                BdwFenceUnref(dependency);BdwFenceUnref(fence);result=-12;return true;
            }
            BdwFenceUnref(dependency);
        }
#endif
        result=worker.SubmitHandoff(resources_,owner,ticket,snapshot,fence,resources_.tokens[ticket.slot]);
        BdwFenceUnref(fence);return true;
    }
};
}
