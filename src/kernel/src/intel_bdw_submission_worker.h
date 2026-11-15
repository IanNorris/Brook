#pragma once
#include "intel_bdw_submission_queue.h"

namespace brook {
// skipped: never executed because a wait dependency failed. Its fences fail
// (error propagation) but it touched no GPU state, so neither its context
// nor its buffers are poisoned: one client's failed frame must not make a
// compositor job that sampled its buffer, or the compositor itself, unusable.
// parked (VA-API phase 4): the job's device work is in flight on another
// engine (VCS); the worker parks the request and completes it later through
// pollParked, running only provably independent work meanwhile.
struct BdwSubmissionOutcome {bool success=false,safelyStopped=false,skipped=false,parked=false;};
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
        // Optional: a job failed (kind 0 skipped, 1 failed, 2 unsafe; n-th of its kind).
        void (*failure)(void*,BdwExecSnapshot*,unsigned kind,uint64_t n)=nullptr;
        // Optional prompt wake. wakeGeneration is sampled before each Step;
        // idle then blocks until wake has been called since that sample or
        // its own bounded timeout expires, replacing sleep. wake is called
        // after work is admitted and may run on any submitting thread.
        uint64_t (*wakeGeneration)(void*)=nullptr;
        void (*idle)(void*,uint64_t)=nullptr;
        void (*wake)(void*)=nullptr;
        // Optional (required to accept a parked outcome): complete the parked
        // job if its device work is done. True with the final outcome in out;
        // false while still in flight. Runs on the worker thread.
        bool (*pollParked)(void*,BdwExecSnapshot*,BdwSubmissionOutcome& out)=nullptr;
        // Optional: idle with a parked job. True if it waited (e.g. a yield
        // for a quicker breadcrumb poll); false uses the ordinary idle wait.
        bool (*parkedIdle)(void*)=nullptr;
    };
private:
    BdwSubmissionQueue queue_;Hooks hooks_{};uint32_t state_=0; // fresh, starting, ready, failed
    uint32_t fairService_=0;bool serviceDeferred_=false; // deferred_ is worker-thread-only
    uint64_t parkedId_=0;BdwExecSnapshot* parkedSnapshot_=nullptr; // worker-thread-only
    uint64_t parkedTotal_=0,overlappedJobs_=0;
    uint64_t pendingCount_=0,bypassCount_=0,noEligibleCount_=0,forcedServiceCount_=0;
    uint64_t failures_[3]{};
    static void Run(void* opaque){
        auto& self=*static_cast<BdwSubmissionWorker*>(opaque);
        const bool prompt=self.hooks_.wakeGeneration && self.hooks_.idle;
        for(;;){
            // Sample before Step so work admitted after Step's queue check
            // is never slept through; at worst it costs one extra Step.
            const uint64_t seen=prompt?self.hooks_.wakeGeneration(self.hooks_.context):0;
            const auto step=self.Step();
            if(step==BdwSubmissionStep::Recover){self.hooks_.recover(self.hooks_.context);__builtin_trap();}
            if(step==BdwSubmissionStep::Idle){
                if(self.parkedId_ && self.hooks_.parkedIdle && self.hooks_.parkedIdle(self.hooks_.context)){}
                else if(prompt)self.hooks_.idle(self.hooks_.context,seen);
                else self.hooks_.sleep(self.hooks_.context);
            }
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
        const auto result=queue_.Submit(snapshot);
        if(!result)Wake();
        return result;
    }
    // Any thread: new work or service requests may be ready.
    void Wake(){if(__atomic_load_n(&state_,__ATOMIC_ACQUIRE)==2 && hooks_.wake)hooks_.wake(hooks_.context);}
    BdwFence* AcquireBufferFence(BdwGpuBuffer* buffer){
        return __atomic_load_n(&state_,__ATOMIC_ACQUIRE)==2?queue_.AcquireBufferFence(buffer):nullptr;
    }
    int64_t BeginPreparation(uint64_t owner,BdwSubmissionPreparation& entry){
        if(__atomic_load_n(&state_,__ATOMIC_ACQUIRE)!=2)return -19;
        return queue_.BeginPreparation(owner,entry);
    }
    void EndPreparation(BdwSubmissionPreparation& entry){queue_.EndPreparation(entry);}
    // See BdwSubmissionQueue::ReservePreparation; -11 means wait for space.
    int64_t ReservePreparation(uint64_t owner,uint64_t presentOwner,BdwSubmissionPreparation& entry){
        if(__atomic_load_n(&state_,__ATOMIC_ACQUIRE)!=2)return -19;
        return queue_.ReservePreparation(owner,presentOwner,entry);
    }
    BdwQueueSpaceStats SpaceStats(){return __atomic_load_n(&state_,__ATOMIC_ACQUIRE)==2?queue_.SpaceStats():BdwQueueSpaceStats{};}
    int64_t SubmitHandoff(BdwHandoffResources& handoff,uint64_t owner,HandoffTicket ticket,
                          BdwExecSnapshot* snapshot,BdwFence* fence,uint64_t gateToken){
        if(__atomic_load_n(&state_,__ATOMIC_ACQUIRE)!=2)return -19;
        const auto result=queue_.SubmitHandoff(handoff,owner,ticket,snapshot,fence,gateToken);
        if(!result)Wake();
        return result;
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
        const bool thawed=__atomic_load_n(&state_,__ATOMIC_ACQUIRE)==2 && queue_.ThawOwner(owner);
        if(thawed)Wake();
        return thawed;
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
    // Failed jobs by kind: skipped (a dependency failed), failed (executed,
    // safely stopped) and unsafe; the first few are logged by the hook.
    uint64_t FailureCount(unsigned kind)const{return kind<3?__atomic_load_n(&failures_[kind],__ATOMIC_RELAXED):0;}
    void ServiceFairnessCounts(uint64_t& pending,uint64_t& bypass,uint64_t& noEligible,uint64_t& forced)const{
        pending=__atomic_load_n(&pendingCount_,__ATOMIC_RELAXED);
        bypass=__atomic_load_n(&bypassCount_,__ATOMIC_RELAXED);
        noEligible=__atomic_load_n(&noEligibleCount_,__ATOMIC_RELAXED);
        forced=__atomic_load_n(&forcedServiceCount_,__ATOMIC_RELAXED);
    }
    void NoteFailure(BdwExecSnapshot* snapshot,const BdwSubmissionOutcome& r){
        const unsigned kind=r.skipped?0:r.safelyStopped?1:2;
        const uint64_t n=__atomic_add_fetch(&failures_[kind],uint64_t{1},__ATOMIC_RELAXED);
        if(hooks_.failure)hooks_.failure(hooks_.context,snapshot,kind,n);
    }
    // Worker thread only: finish a parked job whose device work is done.
    // True if nothing remains parked afterwards.
    bool CompleteParked(BdwSubmissionStep& step){
        step=BdwSubmissionStep::Idle;
        if(!parkedId_)return true;
        BdwSubmissionOutcome out{};
        if(!hooks_.pollParked || !hooks_.pollParked(hooks_.context,parkedSnapshot_,out))return false;
        if(!out.success)NoteFailure(parkedSnapshot_,out);
        const uint64_t id=parkedId_;parkedId_=0;parkedSnapshot_=nullptr;
        step=queue_.Finish(id,out.success,out.safelyStopped,out.skipped)?BdwSubmissionStep::Finished:BdwSubmissionStep::Recover;
        return true;
    }
    bool HasParked()const{return parkedId_!=0;}
    void ParkedCounts(uint64_t& parked,uint64_t& overlapped)const{parked=parkedTotal_;overlapped=overlappedJobs_;}
    // Single-consumer only. Exposed separately for deterministic worker tests.
    BdwSubmissionStep Step(){
        if(parkedId_){BdwSubmissionStep done{};if(CompleteParked(done) && done!=BdwSubmissionStep::Idle)return done;}
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
        if(parkedId_)++overlappedJobs_;
        const auto result=BdwExecSnapshotDependencies(snapshot)==2?BdwSubmissionOutcome{false,true,true}:
            hooks_.execute(hooks_.context,snapshot);
        if(result.parked){
            if(!hooks_.pollParked || parkedId_ || !queue_.Park(id))return BdwSubmissionStep::Recover;
            parkedId_=id;parkedSnapshot_=snapshot;++parkedTotal_;return BdwSubmissionStep::Finished;
        }
        if(!result.success)NoteFailure(snapshot,result);
        return queue_.Finish(id,result.success,result.safelyStopped,result.skipped)?BdwSubmissionStep::Finished:BdwSubmissionStep::Recover;
    }
};
// Launch a permanent CPU-0 kernel worker with Brook scheduler/recovery hooks.
// The caller retains worker/context forever and provides the device executor.
bool BdwStartKernelSubmissionWorker(BdwSubmissionWorker& worker,void* context,
                                    BdwSubmissionOutcome (*execute)(void*,BdwExecSnapshot*),bool knownOwnersOnly=false,
                                    bool (*service)(void*,BdwSubmissionQueue&)=nullptr,
                                    bool (*servicePending)(void*)=nullptr);
}
