#pragma once
#include "intel_bdw_submission_queue.h"

namespace brook {
struct BdwSubmissionOutcome {bool success=false,safelyStopped=false;};
enum class BdwSubmissionStep:uint8_t { Idle,Finished,Recover };
// A permanent single-engine worker. The supplied launch hook must create a
// kernel thread pinned to the requested CPU; this object and hook context
// must outlive that thread. execute owns the device lease, preparation,
// command safety, cleanup and failure diagnostics. A false safelyStopped
// quarantines the job; recovery must not return. Nothing here publishes an
// ioctl or grants untrusted GPU command execution.
class BdwSubmissionWorker {
public:
    struct Hooks {
        void* context=nullptr;
        bool (*launch)(void*,void (*)(void*),void*,uint32_t)=nullptr;
        void (*sleep)(void*)=nullptr;
        BdwSubmissionOutcome (*execute)(void*,BdwExecSnapshot*)=nullptr;
        void (*recover)(void*)=nullptr;
        bool (*service)(void*,BdwSubmissionQueue&)=nullptr;
        bool (*servicePending)(void*)=nullptr;
    };
private:
    BdwSubmissionQueue queue_;Hooks hooks_{};uint32_t state_=0; // fresh, starting, ready, failed
    uint32_t fairService_=0;bool serviceDeferred_=false; // deferred_ is worker-thread-only
    uint64_t pendingCount_=0,bypassCount_=0,noEligibleCount_=0,forcedServiceCount_=0;
    static void Run(void* opaque){
        auto& self=*static_cast<BdwSubmissionWorker*>(opaque);
        for(;;){
            const auto step=self.Step();
            if(step==BdwSubmissionStep::Recover){self.hooks_.recover(self.hooks_.context);__builtin_trap();}
            if(step==BdwSubmissionStep::Idle)self.hooks_.sleep(self.hooks_.context);
        }
    }
public:
    BdwSubmissionWorker()=default;
    BdwSubmissionWorker(const BdwSubmissionWorker&)=delete;
    BdwSubmissionWorker& operator=(const BdwSubmissionWorker&)=delete;
    bool Start(const Hooks& hooks,uint32_t cpu,bool knownOwnersOnly=false){
        uint32_t expected=0;
        if(!hooks.launch || !hooks.sleep || !hooks.execute || !hooks.recover ||
           !__atomic_compare_exchange_n(&state_,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return false;
        hooks_=hooks;
        if(!queue_.Init(knownOwnersOnly) || !hooks_.launch(hooks_.context,Run,this,cpu)){
            __atomic_store_n(&state_,3,__ATOMIC_RELEASE);return false;
        }
        __atomic_store_n(&state_,2,__ATOMIC_RELEASE);return true;
    }
    int64_t Submit(BdwExecSnapshot* snapshot){
        if(__atomic_load_n(&state_,__ATOMIC_ACQUIRE)!=2)return -19;
        return queue_.Submit(snapshot);
    }
    BdwFence* AcquireBufferFence(BdwGpuBuffer* buffer){
        return __atomic_load_n(&state_,__ATOMIC_ACQUIRE)==2?queue_.AcquireBufferFence(buffer):nullptr;
    }
    int64_t BeginPreparation(uint64_t owner,BdwSubmissionPreparation& entry){
        if(__atomic_load_n(&state_,__ATOMIC_ACQUIRE)!=2)return -19;
        return queue_.BeginPreparation(owner,entry);
    }
    void EndPreparation(BdwSubmissionPreparation& entry){queue_.EndPreparation(entry);}
    int64_t SubmitHandoff(BdwHandoffResources& handoff,uint64_t owner,HandoffTicket ticket,
                          BdwExecSnapshot* snapshot,BdwFence* fence,uint64_t gateToken){
        if(__atomic_load_n(&state_,__ATOMIC_ACQUIRE)!=2)return -19;
        return queue_.SubmitHandoff(handoff,owner,ticket,snapshot,fence,gateToken);
    }
    int64_t CancelBlockedHandoff(BdwHandoffResources& handoff,uint64_t owner,HandoffTicket ticket){
        if(__atomic_load_n(&state_,__ATOMIC_ACQUIRE)!=2)return -19;
        return queue_.CancelBlockedHandoff(handoff,owner,ticket);
    }
    bool FreezeOwner(uint64_t owner,BdwExecOwnerWork& out){
        return __atomic_load_n(&state_,__ATOMIC_ACQUIRE)==2 && queue_.FreezeOwner(owner,out);
    }
    bool InspectOwner(uint64_t owner,BdwExecOwnerWork& out){
        return __atomic_load_n(&state_,__ATOMIC_ACQUIRE)==2 && queue_.InspectOwner(owner,out);
    }
    bool ThawOwner(uint64_t owner){
        return __atomic_load_n(&state_,__ATOMIC_ACQUIRE)==2 && queue_.ThawOwner(owner);
    }
    bool IsFrozenOwner(uint64_t owner){
        return __atomic_load_n(&state_,__ATOMIC_ACQUIRE)==2 && queue_.IsFrozenOwner(owner);
    }
    bool FrozenOwnerWork(uint64_t owner,BdwExecOwnerWork& out){
        return __atomic_load_n(&state_,__ATOMIC_ACQUIRE)==2 && queue_.FrozenOwnerWork(owner,out);
    }
    int64_t FreezeAndCancelBlockedHandoff(BdwHandoffResources& handoff,uint64_t owner,HandoffTicket ticket){
        if(__atomic_load_n(&state_,__ATOMIC_ACQUIRE)!=2)return -19;
        return queue_.FreezeAndCancelBlockedHandoff(handoff,owner,ticket);
    }
    int64_t CancelFrozenOwnerQueued(BdwHandoffResources& handoff,uint64_t owner){
        if(__atomic_load_n(&state_,__ATOMIC_ACQUIRE)!=2)return -19;
        return queue_.CancelFrozenOwnerQueued(handoff,owner);
    }
    static int64_t SubmitCallback(void* context,BdwExecSnapshot* snapshot){
        return context?static_cast<BdwSubmissionWorker*>(context)->Submit(snapshot):-19;
    }
    bool SetServiceFairness(bool enabled){
        if(__atomic_load_n(&state_,__ATOMIC_ACQUIRE)!=2 || (enabled && (!hooks_.service || !hooks_.servicePending)))return false;
        __atomic_store_n(&fairService_,enabled?1u:0u,__ATOMIC_RELEASE);return true;
    }
    bool ServiceFairness()const{return __atomic_load_n(&fairService_,__ATOMIC_ACQUIRE)!=0;}
    void ServiceFairnessCounts(uint64_t& pending,uint64_t& bypass,uint64_t& noEligible,uint64_t& forced)const{
        pending=__atomic_load_n(&pendingCount_,__ATOMIC_RELAXED);
        bypass=__atomic_load_n(&bypassCount_,__ATOMIC_RELAXED);
        noEligible=__atomic_load_n(&noEligibleCount_,__ATOMIC_RELAXED);
        forced=__atomic_load_n(&forcedServiceCount_,__ATOMIC_RELAXED);
    }
    // Single-consumer only. Exposed separately for deterministic worker tests.
    BdwSubmissionStep Step(){
        uint64_t id=0;BdwExecSnapshot* snapshot=nullptr;
        const bool fair=__atomic_load_n(&fairService_,__ATOMIC_ACQUIRE) && hooks_.servicePending;
        const bool pending=hooks_.servicePending && hooks_.servicePending(hooks_.context);
        if(pending)__atomic_fetch_add(&pendingCount_,uint64_t{1},__ATOMIC_RELAXED);
        bool taken=false;
        if(fair && pending && !serviceDeferred_){
            if(queue_.Take(id,snapshot,true)){
                __atomic_fetch_add(&bypassCount_,uint64_t{1},__ATOMIC_RELAXED);serviceDeferred_=true;taken=true;
            }else __atomic_fetch_add(&noEligibleCount_,uint64_t{1},__ATOMIC_RELAXED);
        }
        if(!taken){
            if(hooks_.service && hooks_.service(hooks_.context,queue_)){
                if(serviceDeferred_)__atomic_fetch_add(&forcedServiceCount_,uint64_t{1},__ATOMIC_RELAXED);
                serviceDeferred_=false;return BdwSubmissionStep::Finished;
            }
            if(!pending)serviceDeferred_=false;
            if(!queue_.Take(id,snapshot))return queue_.Poisoned()?BdwSubmissionStep::Recover:BdwSubmissionStep::Idle;
        }
        const auto result=BdwExecSnapshotDependencies(snapshot)==2?BdwSubmissionOutcome{false,true}:
            hooks_.execute(hooks_.context,snapshot);
        return queue_.Finish(id,result.success,result.safelyStopped)?BdwSubmissionStep::Finished:BdwSubmissionStep::Recover;
    }
};
// Launch a permanent CPU-0 kernel worker with Brook scheduler/recovery hooks.
// The caller retains worker/context forever and provides the device executor.
bool BdwStartKernelSubmissionWorker(BdwSubmissionWorker& worker,void* context,
                                    BdwSubmissionOutcome (*execute)(void*,BdwExecSnapshot*),bool knownOwnersOnly=false,
                                    bool (*service)(void*,BdwSubmissionQueue&)=nullptr,
                                    bool (*servicePending)(void*)=nullptr);
}
