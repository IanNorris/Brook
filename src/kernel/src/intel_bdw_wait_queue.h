#pragma once
#include <stdint.h>

namespace brook {
// Bounded one-shot waiter registry with a wake generation. Pure logic: the
// caller serializes every call with one lock and performs the wake callback
// while still holding it, so a waiter can never be woken after it disarms.
// A waiter observes the generation before checking its condition and arms
// with that value; a signal in between makes Arm report Stale instead of
// letting the waiter sleep through it. Each arming is woken at most once.
enum class BdwWaitArm : uint8_t { Armed, Stale, Full };
template<class Waiter,unsigned Capacity> class BdwWaitSlots {
    static_assert(Capacity>0 && Capacity<=64,"bounded waiter scan");
    Waiter* slots_[Capacity]{};
    uint64_t generation_=0;
public:
    uint64_t Generation()const{return __atomic_load_n(&generation_,__ATOMIC_ACQUIRE);}
    BdwWaitArm Arm(Waiter* waiter,uint64_t seen,unsigned& slot){
        slot=Capacity;
        if(!waiter || generation_!=seen)return BdwWaitArm::Stale;
        for(unsigned i=0;i<Capacity;++i)if(!slots_[i]){slots_[i]=waiter;slot=i;return BdwWaitArm::Armed;}
        return BdwWaitArm::Full;
    }
    // False when a signal already consumed this arming (or it never armed).
    bool Disarm(Waiter* waiter,unsigned slot){
        if(slot>=Capacity || !waiter || slots_[slot]!=waiter)return false;
        slots_[slot]=nullptr;return true;
    }
    template<class Wake> unsigned Signal(Wake&& wake){
        __atomic_store_n(&generation_,generation_+1,__ATOMIC_RELEASE);
        unsigned woken=0;
        for(unsigned i=0;i<Capacity;++i)if(auto* waiter=slots_[i]){slots_[i]=nullptr;wake(waiter);++woken;}
        return woken;
    }
    unsigned Armed()const{unsigned n=0;for(auto* w:slots_)n+=w!=nullptr;return n;}
};

// Kernel GPU wake events. Work wakes the gpu-submit worker (submission,
// present request, thaw, fence signal). Retire wakes CPU-side waiters on
// fences, buffer GPU use, sync-object publication and BCS presentation.
// Space wakes execbuffer callers waiting for a submission-queue slot; it is
// signaled only while such a waiter is registered with the queue.
// Every wait keeps a one-tick timeout as a safety net, so a missing signal
// degrades to the previous 1 ms polling rather than hanging.
enum class BdwGpuEvent : uint32_t { Work=0, Retire=1, Space=2, Count=3 };
struct BdwGpuWaiter {uint32_t event=0;unsigned slot=~0u;void* process=nullptr;};
#ifndef BROOK_HOST_TEST
uint64_t BdwGpuEventGeneration(BdwGpuEvent event);
// Armed: caller must block (bounded) and then Disarm. Stale: re-check now.
// Full: no slot; caller falls back to its plain timed sleep.
BdwWaitArm BdwGpuEventArm(BdwGpuEvent event,uint64_t seen,BdwGpuWaiter& waiter);
void BdwGpuEventDisarm(BdwGpuWaiter& waiter);
void BdwGpuEventSignal(BdwGpuEvent event, uint64_t fenceId = 0);
// Uninterruptible: block the current kernel-context thread for at most one
// scheduler tick or until the event's generation moves past seen.
void BdwGpuEventWaitTick(BdwGpuEvent event,uint64_t seen);
#else
// Host builds keep the polling behaviour: nothing ever arms.
inline uint64_t& BdwHostGpuEventGeneration(BdwGpuEvent event){
    static uint64_t generations[uint32_t(BdwGpuEvent::Count)]{};return generations[uint32_t(event)];
}
inline uint64_t BdwGpuEventGeneration(BdwGpuEvent event){return __atomic_load_n(&BdwHostGpuEventGeneration(event),__ATOMIC_ACQUIRE);}
inline BdwWaitArm BdwGpuEventArm(BdwGpuEvent event,uint64_t seen,BdwGpuWaiter&){
    return BdwGpuEventGeneration(event)!=seen?BdwWaitArm::Stale:BdwWaitArm::Full;
}
inline void BdwGpuEventDisarm(BdwGpuWaiter&){}
inline void BdwGpuEventSignal(BdwGpuEvent event,uint64_t = 0){__atomic_fetch_add(&BdwHostGpuEventGeneration(event),uint64_t{1},__ATOMIC_ACQ_REL);}
inline void BdwGpuEventWaitTick(BdwGpuEvent,uint64_t){}
#endif
// A fence completion can release CPU waiters and queued dependencies.
inline void BdwGpuFenceSignalled(uint64_t fenceId = 0) {
    BdwGpuEventSignal(BdwGpuEvent::Retire, fenceId);
    BdwGpuEventSignal(BdwGpuEvent::Work, fenceId);
}
}
