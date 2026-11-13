#pragma once
#include <stdint.h>
#include "intel_bdw_exec_snapshot.h"
#include "intel_bdw_buffer.h"
#include "intel_bdw_fence.h"

namespace brook {
struct BdwExecSnapshot;
enum class BdwExecRequestState : uint8_t { Free,Queued,Active,Cancelled,Complete,Failed,Quarantined };
struct BdwExecRequestView {uint64_t id=0,owner=0;BdwExecRequestState state=BdwExecRequestState::Free;};
struct BdwExecOwnerWork {uint32_t queued=0,active=0,quarantined=0,unknown=0,retiring=0,preparing=0;};

// A bounded ownership ledger for one serialized render engine. The caller
// holds an external lock for every operation, supplies globally unique live
// owner IDs and wakes the worker after successful insertion. Insertion
// reserves buffer use before transferring the snapshot reference to the
// ledger; completion or cancellation retires its buffer use, while reaping
// transfers the retained snapshot record to a caller that must release it.
// Active or quarantined snapshots cannot be reaped. This class neither
// schedules a worker nor submits commands, and it does not itself release
// snapshots or validate GPU memory, privilege, cache or reset safety.
class BdwExecQueue {
    static constexpr unsigned Capacity=32;
    struct Entry {uint64_t id=0,owner=0;BdwExecSnapshot* snapshot=nullptr;BdwExecRequestState state=BdwExecRequestState::Free;};
    Entry entries_[Capacity]{};
    uint64_t next_=1,active_=0;bool exhausted_=false,poisoned_=false;
    Entry* find(uint64_t id){for(auto& e:entries_)if(e.state!=BdwExecRequestState::Free && e.id==id)return &e;return nullptr;}
public:
    BdwExecQueue()=default;
    BdwExecQueue(const BdwExecQueue&)=delete;
    BdwExecQueue& operator=(const BdwExecQueue&)=delete;
    bool Poisoned()const{return poisoned_;}
    // Capture an immutable join of exactly the requests already admitted for
    // this backing. Caller holds the queue lock and must exclude new producer
    // writes; later consumer submissions do not become fence dependencies.
    // A non-queue reservation, unknown failure or allocation failure refuses
    // the snapshot rather than reporting false completion.
    BdwFence* AcquireBufferFence(BdwGpuBuffer* buffer)const{
        if(!buffer || poisoned_)return nullptr;
        uint32_t uses=0;bool failed=false;BdwGpuBufferGpuStatus(buffer,uses,failed);
        if(failed)return nullptr;
        auto* joined=BdwFenceCreate(true,nullptr,0,nullptr,0,uses!=0);if(!joined)return nullptr;
        uint32_t represented=0;
        for(const auto& e:entries_)if(e.state==BdwExecRequestState::Queued || e.state==BdwExecRequestState::Active || e.state==BdwExecRequestState::Quarantined){
            const uint32_t n=BdwExecSnapshotBufferUseCount(e.snapshot,buffer);if(!n)continue;
            if(n>~uint32_t{0}-represented){BdwFenceUnref(joined);return nullptr;}
            auto* dependency=BdwExecSnapshotAcquireCompletionForBuffer(e.snapshot,buffer);
            if(!dependency){BdwFenceUnref(joined);return nullptr;}
            auto* next=BdwFenceCreate(true,joined,0,dependency);BdwFenceUnref(dependency);BdwFenceUnref(joined);
            if(!next)return nullptr;joined=next;represented+=n;
        }
        if(represented!=uses){BdwFenceUnref(joined);return nullptr;}
        return joined;
    }
    // Snapshot only: new admissions can race immediately after unlocking.
    // Unknown work must prevent callers from inferring process quiescence.
    bool InspectOwner(uint64_t owner,BdwExecOwnerWork& out)const{
        if(!owner)return false;out={};
        for(const auto& e:entries_){
            if(e.state!=BdwExecRequestState::Queued && e.state!=BdwExecRequestState::Active && e.state!=BdwExecRequestState::Quarantined)continue;
            const auto recorded=BdwExecSnapshotOwner(e.snapshot);
            if(!recorded){++out.unknown;continue;}
            if(recorded!=owner)continue;
            if(e.state==BdwExecRequestState::Queued)++out.queued;
            else if(e.state==BdwExecRequestState::Active)++out.active;
            else ++out.quarantined;
        }
        return true;
    }
    bool Enqueue(uint64_t owner,BdwExecSnapshot* snapshot,uint64_t& id){
        if(!owner || !snapshot || poisoned_ || exhausted_)return false;
        for(const auto& e:entries_)if(e.state!=BdwExecRequestState::Free && e.snapshot==snapshot)return false;
        for(auto& e:entries_)if(e.state==BdwExecRequestState::Free){
            if(!BdwExecSnapshotReserve(snapshot))return false;
            const uint64_t issued=next_;if(next_==~uint64_t{0})exhausted_=true;else ++next_;
            e={issued,owner,snapshot,BdwExecRequestState::Queued};id=issued;return true;
        }
        return false;
    }
    bool Observe(uint64_t owner,uint64_t id,BdwExecRequestView& out){
        auto* e=find(id);if(!e || !owner || e->owner!=owner)return false;
        out={e->id,e->owner,e->state};return true;
    }
    bool Cancel(uint64_t owner,uint64_t id){
        auto* e=find(id);if(!e || !owner || e->owner!=owner || e->state!=BdwExecRequestState::Queued)return false;
        if(!BdwExecSnapshotRetire(e->snapshot,false))return false;
        if(!BdwExecSnapshotSignalFences(e->snapshot,false)){
            // Retirement already changed snapshot state. Never leave it in
            // the runnable queue if signaling violated an invariant.
            e->state=BdwExecRequestState::Quarantined;poisoned_=true;return false;
        }
        e->state=BdwExecRequestState::Cancelled;return true;
    }
    // A renderer may abandon a request stalled on a dependency, but this
    // must never cancel active work or a queued request that is ready to run.
    // Caller holds the queue lock across validation and cancellation.
    bool CancelBlocked(uint64_t owner,uint64_t id){
        auto* e=find(id);
        return e && owner && e->owner==owner && e->state==BdwExecRequestState::Queued &&
            BdwExecSnapshotDependencies(e->snapshot)==0 && Cancel(owner,id);
    }
    bool CancelBlockedForOrigin(uint64_t queueOwner,uint64_t id,uint64_t incarnation){
        auto* e=find(id);
        return incarnation && e && e->owner==queueOwner &&
            BdwExecSnapshotOwner(e->snapshot)==incarnation && CancelBlocked(queueOwner,id);
    }
    // A frozen owner's queued records have never reached the engine. Cancel
    // all of them (including ready ones) without disturbing another owner.
    bool CancelQueuedForOrigin(uint64_t queueOwner,uint64_t incarnation,uint64_t (&ids)[32],uint32_t& count){
        count=0;if(!queueOwner || !incarnation)return false;
        for(auto& e:entries_)if(e.state==BdwExecRequestState::Queued && e.owner==queueOwner &&
                                  BdwExecSnapshotOwner(e.snapshot)==incarnation){
            if(!Cancel(queueOwner,e.id))return false;
            ids[count++]=e.id;
        }
        return true;
    }
    bool StartNext(uint64_t& id,BdwExecSnapshot*& snapshot,bool applicationOnly=false){
        if(poisoned_ || active_)return false;
        Entry* first=nullptr;
        for(auto& e:entries_)if(e.state==BdwExecRequestState::Queued && BdwExecSnapshotDependencies(e.snapshot) &&
                                  (!applicationOnly || !BdwExecSnapshotHasProtectedBuffer(e.snapshot)) && (!first || e.id<first->id)){
            const auto owner=BdwExecSnapshotOwner(e.snapshot);bool permitted=!applicationOnly || owner;
            for(const auto& prior:entries_)if(prior.state==BdwExecRequestState::Queued && prior.id<e.id){
                const auto previous=BdwExecSnapshotOwner(prior.snapshot);
                if(!owner || !previous || owner==previous || !BdwExecSnapshotsIndependent(prior.snapshot,e.snapshot)){
                    permitted=false;break;
                }
            }
            if(permitted)first=&e;
        }
        if(!first)return false;
        first->state=BdwExecRequestState::Active;active_=first->id;id=first->id;snapshot=first->snapshot;return true;
    }
    // Only the engine owner may call Finish. safelyStopped requires verified
    // retirement/reset and all necessary device/cache cleanup, not a timeout.
    bool Finish(uint64_t id,bool success,bool safelyStopped){
        auto* e=find(id);if(!e || e->state!=BdwExecRequestState::Active || active_!=id)return false;
        // Publish failure before any use count can fall, so waiters cannot
        // mistake failed work for successful idle buffers. An unsafe engine
        // cannot process its queued requests; fail them while retaining their
        // snapshots until the owner reaps and releases them.
        if((!success || !safelyStopped) && !BdwExecSnapshotMarkFailed(e->snapshot))return false;
        if(!safelyStopped)for(auto& pending:entries_)if(pending.state==BdwExecRequestState::Queued){
            if(!BdwExecSnapshotMarkFailed(pending.snapshot,true))return false;
            if(!BdwExecSnapshotRetire(pending.snapshot,false))return false;
            if(!BdwExecSnapshotSignalFences(pending.snapshot,false))return false;
            pending.state=BdwExecRequestState::Failed;
        }
        if(safelyStopped && !BdwExecSnapshotRetire(e->snapshot,true)){
            // A cache synchronization failure must never look like success.
            // Keep the active snapshot retained and poison the queue.
            if(!BdwExecSnapshotMarkFailed(e->snapshot))return false;
            for(auto& pending:entries_)if(pending.state==BdwExecRequestState::Queued){
                if(!BdwExecSnapshotMarkFailed(pending.snapshot,true) || !BdwExecSnapshotRetire(pending.snapshot,false) ||
                   !BdwExecSnapshotSignalFences(pending.snapshot,false))return false;
                pending.state=BdwExecRequestState::Failed;
            }
            active_=0;e->state=BdwExecRequestState::Quarantined;poisoned_=true;return true;
        }
        active_=0;
        if(!safelyStopped){e->state=BdwExecRequestState::Quarantined;poisoned_=true;}
        else {if(!BdwExecSnapshotSignalFences(e->snapshot,success))return false;
            e->state=success?BdwExecRequestState::Complete:BdwExecRequestState::Failed;}
        return true;
    }
    BdwExecSnapshot* Reap(uint64_t owner,uint64_t id,BdwExecRequestState& state){
        auto* e=find(id);if(!e || !owner || e->owner!=owner ||
            (e->state!=BdwExecRequestState::Cancelled && e->state!=BdwExecRequestState::Complete && e->state!=BdwExecRequestState::Failed))return nullptr;
        auto* snapshot=e->snapshot;state=e->state;*e={};return snapshot;
    }
    // Internal service reaper; caller holds the same queue lock. Active and
    // quarantined requests remain owned, regardless of their owner ID.
    BdwExecSnapshot* ReapAnyFinished(){
        for(auto& e:entries_)if(e.state==BdwExecRequestState::Cancelled || e.state==BdwExecRequestState::Complete || e.state==BdwExecRequestState::Failed){
            auto* snapshot=e.snapshot;e={};return snapshot;
        }
        return nullptr;
    }
};
}
