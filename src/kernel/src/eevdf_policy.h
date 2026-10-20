#pragma once
#include "sched_ops.h"

namespace brook {
// Original Brook EEVDF core, not a Linux scheduler port. Fixed global storage,
// caller serialization, equal initial weight 1024; request length follows the
// existing priority hint. No userspace nice/weight API is introduced here.
struct EevdfDivision { uint64_t quotient, remainder; bool overflow; };
inline EevdfDivision EevdfDivide(unsigned __int128 numerator, uint64_t denominator) {
    if (!denominator) return {~uint64_t(0),0,true};
    const uint64_t high=uint64_t(numerator>>64), low=uint64_t(numerator);
    if (high>=denominator) return {~uint64_t(0),0,true};
    if (!high) return {low/denominator,low%denominator,false};
    uint64_t quotient=0,remainder=high;
    // 128/64 with a 64-bit quotient; no freestanding __udivti3 dependency.
    for (int bit=63;bit>=0;--bit) {
        const bool carry=(remainder>>63)!=0;
        remainder=(remainder<<1)|((low>>bit)&1);
        if (carry || remainder>=denominator) {
            remainder-=denominator;quotient|=uint64_t(1)<<bit;
        }
    }
    return {quotient,remainder,false};
}
struct EevdfTaskSnapshot {
    int64_t virtualRuntime=0,deadline=0,savedLag=0;
    uint32_t weight=0,remainingNs=0,requestNs=0;
    bool active=false,queued=false,runnable=false,deferred=false;
};
struct EevdfStatistics {
    uint64_t serviceNs=0,inactiveServiceNs=0,rebases=0,saturations=0,orderRebases=0;
    uint64_t deferredRemovals=0,workConservingPicks=0;
};
class EevdfPolicy {
    static constexpr uint16_t None=SCHED_PID_NONE;
    static constexpr int64_t Maximum=int64_t(~uint64_t(0)>>1), Minimum=-Maximum-1;
    static constexpr int64_t RebaseLimit=int64_t(1)<<50;
    struct Task {
        int64_t v=0,deadline=0,lag=0;
        uint64_t order=0;
        uint32_t weight=0,request=0,remaining=0,fraction=0;
        uint16_t next=None,prev=None,heapIndex=None;
        bool active:1,member:1,runnable:1,queued:1,deferred:1,entered:1;
    } tasks_[SCHED_MAX_PIDS];
    uint16_t head_=None;
    uint16_t deferredHeap_[SCHED_MAX_PIDS]{};
    uint16_t deferredCount_=0;
    __int128 weightedRuntime_=0;
    uint64_t totalWeight_=0;
    uint32_t ready_=0;
    uint64_t sequence_=0;
    EevdfStatistics stats_{};

    bool Valid(uint16_t pid) const {return pid && pid<SCHED_MAX_PIDS && tasks_[pid].active;}
    int64_t Safe(__int128 value) {
        if(value>Maximum){++stats_.saturations;return Maximum;}
        if(value<Minimum){++stats_.saturations;return Minimum;}
        return int64_t(value);
    }
    int64_t DivideSigned(__int128 value,uint64_t denominator) const {
        if(!denominator)return 0;
        const bool negative=value<0;
        const auto d=EevdfDivide(negative?static_cast<unsigned __int128>(-value):static_cast<unsigned __int128>(value),denominator);
        // Weighted means and bounded lag placements fit int64 by construction.
        const __int128 answer=negative?-__int128(d.quotient)-(d.remainder!=0):__int128(d.quotient);
        return answer<Minimum?Minimum:(answer>Maximum?Maximum:int64_t(answer));
    }
    int64_t Normalize(uint64_t ns,uint32_t weight) {
        if(weight==1024 && ns<=uint64_t(Maximum))return int64_t(ns);
        const auto result=EevdfDivide(static_cast<unsigned __int128>(ns)*1024,weight);
        if(result.overflow || result.quotient>uint64_t(Maximum)){
            ++stats_.saturations;return Maximum;
        }
        return int64_t(result.quotient);
    }
    bool HeapLess(uint16_t a,uint16_t b) const {
        return tasks_[a].v<tasks_[b].v || (tasks_[a].v==tasks_[b].v && a<b);
    }
    void HeapSwap(unsigned a,unsigned b) {
        const uint16_t value=deferredHeap_[a];deferredHeap_[a]=deferredHeap_[b];deferredHeap_[b]=value;
        tasks_[deferredHeap_[a]].heapIndex=uint16_t(a);tasks_[deferredHeap_[b]].heapIndex=uint16_t(b);
    }
    unsigned HeapUp(unsigned index) {
        while(index && HeapLess(deferredHeap_[index],deferredHeap_[(index-1)/2])){
            const unsigned parent=(index-1)/2;HeapSwap(index,parent);index=parent;
        }
        return index;
    }
    void HeapDown(unsigned index) {
        while(index*2+1<deferredCount_){
            unsigned child=index*2+1;
            if(child+1<deferredCount_ && HeapLess(deferredHeap_[child+1],deferredHeap_[child]))++child;
            if(!HeapLess(deferredHeap_[child],deferredHeap_[index]))break;
            HeapSwap(index,child);index=child;
        }
    }
    void HeapRemove(uint16_t pid) {
        const unsigned index=tasks_[pid].heapIndex;if(index==None)return;
        --deferredCount_;tasks_[pid].heapIndex=None;
        if(index==deferredCount_)return;
        deferredHeap_[index]=deferredHeap_[deferredCount_];tasks_[deferredHeap_[index]].heapIndex=uint16_t(index);
        HeapDown(HeapUp(index));
    }
    void HeapInsert(uint16_t pid) {
        if(tasks_[pid].heapIndex!=None)return;
        const unsigned index=deferredCount_++;deferredHeap_[index]=pid;tasks_[pid].heapIndex=uint16_t(index);HeapUp(index);
    }
    void AssignVirtual(Task& t,int64_t value,bool fixHeap=true) {
        if(t.member)weightedRuntime_+=(__int128(value)-t.v)*t.weight;
        t.v=value;
        if(fixHeap && t.heapIndex!=None)HeapDown(HeapUp(t.heapIndex));
    }
    void Join(uint16_t pid) {
        auto& t=tasks_[pid];if(t.member)return;
        t.prev=None;t.next=head_;if(head_!=None)tasks_[head_].prev=pid;
        head_=pid;t.member=true;weightedRuntime_+=__int128(t.v)*t.weight;totalWeight_+=t.weight;
    }
    void Leave(uint16_t pid) {
        auto& t=tasks_[pid];HeapRemove(pid);if(!t.member)return;
        weightedRuntime_-=__int128(t.v)*t.weight;totalWeight_-=t.weight;
        if(t.prev==None)head_=t.next;else tasks_[t.prev].next=t.next;
        if(t.next!=None)tasks_[t.next].prev=t.prev;
        t.next=t.prev=None;t.member=false;
    }
    uint64_t TotalWeight() const {return totalWeight_;}
    int64_t Mean() const {return DivideSigned(weightedRuntime_,totalWeight_);}
    int64_t LagLimit(Task& t) {return Safe(__int128(Normalize(t.request,t.weight))*2);}
    int64_t ClampLag(Task& t,int64_t lag) {
        const int64_t limit=LagLimit(t);return lag>limit?limit:(lag<-limit?-limit:lag);
    }
    void Renew(Task& t) {
        if(t.remaining)return;
        t.remaining=t.request;t.deadline=Safe(__int128(t.v)+Normalize(t.remaining,t.weight));
    }
    void CleanupDeferred() {
        // Removing a deferred entity at/below the mean raises the mean and can
        // make the next debt expire. A fixed min-heap reaches the fixed point
        // in O(k log Tasks), without repeated whole-policy scans under IF=0.
        while(deferredCount_){
            const uint16_t pid=deferredHeap_[0];auto& t=tasks_[pid];
            if(t.v>Mean())break;
            Leave(pid);t.deferred=false;t.lag=0;++stats_.deferredRemovals;
        }
    }
    void Rebase() {
        const int64_t mean=Mean();if(!mean)return;
        unsigned steps=0;
        for(uint16_t p=head_;p!=None && p<SCHED_MAX_PIDS && steps++<SCHED_MAX_PIDS;p=tasks_[p].next){
            auto& t=tasks_[p];AssignVirtual(t,Safe(__int128(t.v)-mean),false);t.deadline=Safe(__int128(t.deadline)-mean);
        }
        for(unsigned index=deferredCount_/2;index>0;--index)HeapDown(index-1);
        ++stats_.rebases;
    }
    uint16_t Best(uint16_t current) {
        CleanupDeferred();
        const int64_t mean=Mean();int64_t minimum=Maximum;bool eligible=false;
        unsigned steps=0;
        for(uint16_t p=head_;p!=None && p<SCHED_MAX_PIDS && steps++<SCHED_MAX_PIDS;p=tasks_[p].next){
            auto& t=tasks_[p];if(!t.runnable || (!t.queued && p!=current))continue;
            Renew(t);if(t.v<=mean)eligible=true;if(t.v<minimum)minimum=t.v;
        }
        const int64_t frontier=eligible?mean:minimum;
        uint16_t best=None;steps=0;
        for(uint16_t p=head_;p!=None && p<SCHED_MAX_PIDS && steps++<SCHED_MAX_PIDS;p=tasks_[p].next){
            const auto& t=tasks_[p];
            if(!t.runnable || (!t.queued && p!=current) || t.v>frontier)continue;
            if(best==None || t.deadline<tasks_[best].deadline ||
               (t.deadline==tasks_[best].deadline &&
                (t.order<tasks_[best].order || (t.order==tasks_[best].order && p<best))))best=p;
        }
        // Spare CPUs must not idle solely because lower-vruntime tasks already
        // own other CPUs. Advance this available slot's eligibility frontier
        // to the least-served available task; do not erase its service debt.
        if(best!=None && !eligible)++stats_.workConservingPicks;
        return best;
    }
public:
    void Init() {
        for(auto& t:tasks_)t={}; // no whole-policy temporary on the kernel stack
        head_=None;ready_=0;sequence_=0;stats_={};
        deferredCount_=0;weightedRuntime_=0;totalWeight_=0;
        for(auto& entry:deferredHeap_)entry=None;
    }
    void InitProcess(uint16_t pid,uint8_t priority) {
        if(!pid || pid>=SCHED_MAX_PIDS)return;
        auto& t=tasks_[pid];if(t.queued)--ready_;Leave(pid);t={};t.active=true;t.weight=1024;
        const uint32_t requests[]={1000000,2000000,4000000,8000000};
        t.request=requests[priority<4?priority:2];t.remaining=t.request;
    }
    // Test/internal capability only: no live nice/weight control is exposed.
    bool InitialWeight(uint16_t pid,uint32_t weight) {
        if(!Valid(pid) || tasks_[pid].entered || !weight || weight>(1u<<20))return false;
        tasks_[pid].weight=weight;return true;
    }
    void SetRunnable(uint16_t pid,bool runnable) {
        if(!Valid(pid))return;auto& t=tasks_[pid];if(t.runnable==runnable)return;
        CleanupDeferred();
        if(runnable){
            if(!t.member){
                const int64_t mean=Mean();const uint64_t others=TotalWeight();
                const int64_t lag=others?DivideSigned(__int128(t.lag)*(others+t.weight),others):0;
                t.v=Safe(__int128(mean)-lag);
                if(!t.remaining)t.remaining=t.request;
                t.deadline=Safe(__int128(t.v)+Normalize(t.remaining,t.weight));Join(pid);
            }
            HeapRemove(pid);t.runnable=true;t.deferred=false;t.entered=true;
        }else{
            t.lag=ClampLag(t,Safe(__int128(Mean())-t.v));t.runnable=false;
            if(t.lag<0){
                AssignVirtual(t,Safe(__int128(Mean())-t.lag));
                t.deadline=Safe(__int128(t.v)+Normalize(t.remaining,t.weight));t.deferred=true;HeapInsert(pid);
            }else{Leave(pid);t.deferred=false;}
        }
    }
    void Enqueue(uint16_t pid) {
        if(!Valid(pid) || tasks_[pid].queued)return;
        SetRunnable(pid,true);auto& t=tasks_[pid];Renew(t);
        if(sequence_==~uint64_t(0)){
            for(auto& entry:tasks_)entry.order>>=1;
            sequence_>>=1;++stats_.orderRebases;
        }
        t.order=++sequence_;t.queued=true;++ready_;
    }
    uint16_t PickNext() {
        if(!ready_)return None;
        const uint16_t pid=Best(None);if(pid==None)return None;
        tasks_[pid].queued=false;--ready_;return pid;
    }
    void Remove(uint16_t pid) {
        if(Valid(pid) && tasks_[pid].queued){tasks_[pid].queued=false;--ready_;}
    }
    void AccountService(uint16_t pid,uint64_t service) {
        if(!pid || pid>=SCHED_MAX_PIDS || !service)return;
        if(service>~uint64_t(0)-stats_.serviceNs){stats_.serviceNs=~uint64_t(0);++stats_.saturations;}
        else stats_.serviceNs+=service;
        // Hot replacement deliberately does not resurrect terminated entities.
        // An off-CPU retirement still in progress can report its final owned
        // interval afterward: observe that service without creating an entity.
        if(!Valid(pid)){
            if(service>~uint64_t(0)-stats_.inactiveServiceNs){stats_.inactiveServiceNs=~uint64_t(0);++stats_.saturations;}
            else stats_.inactiveServiceNs+=service;
            return;
        }
        auto& t=tasks_[pid];
        const auto division=EevdfDivide(static_cast<unsigned __int128>(service)*1024+t.fraction,t.weight);
        t.fraction=uint32_t(division.remainder);
        int64_t normalized=0;
        if(division.overflow || division.quotient>uint64_t(Maximum)){
            normalized=Maximum;++stats_.saturations;
        }else normalized=int64_t(division.quotient);
        AssignVirtual(t,Safe(__int128(t.v)+normalized));
        t.remaining=service>=t.remaining?0:uint32_t(t.remaining-service);
        if(!t.member)t.lag=ClampLag(t,Safe(__int128(t.lag)-normalized));
        if(t.member && (t.v>RebaseLimit || t.v<-RebaseLimit))Rebase();
    }
    void TimesliceExpired(uint16_t pid) {if(Valid(pid))Renew(tasks_[pid]);}
    void VoluntaryYield(uint16_t) {} // preserve remaining request and debt
    void Tick(uint64_t) {CleanupDeferred();}
    uint64_t Timeslice(uint16_t pid) const {return Valid(pid)?tasks_[pid].request/1000000:4;}
    uint32_t ReadyCount() const {return ready_;}
    bool ShouldPreempt(uint16_t current,uint64_t) {
        if(!Valid(current) || !tasks_[current].runnable)return true;
        const uint16_t best=Best(current);return best!=None && best!=current;
    }
    EevdfTaskSnapshot Inspect(uint16_t pid) const {
        if(!Valid(pid))return {};const auto& t=tasks_[pid];
        return {t.v,t.deadline,t.lag,t.weight,t.remaining,t.request,t.active,t.queued,t.runnable,t.deferred};
    }
    EevdfStatistics Statistics() const {return stats_;}
    int64_t VirtualTime() const {return Mean();}
    void DebugDump(uint16_t pid,SchedDebugInfo* out) const {
        if(!out)return;*out={};out->head=out->tail=None;out->nextPid=out->prevPid=None;
        out->readyCount=ready_;unsigned steps=0;
        for(uint16_t p=head_;p!=None && p<SCHED_MAX_PIDS && steps++<SCHED_MAX_PIDS;p=tasks_[p].next)
            if(tasks_[p].queued){if(out->head==None)out->head=p;out->tail=p;++out->listLen;}
        if(Valid(pid)){out->active=1;out->queued=tasks_[pid].queued;}
        out->hasServiceInfo=true;out->serviceNs=stats_.serviceNs;out->inactiveServiceNs=stats_.inactiveServiceNs;
        out->rebases=stats_.rebases;out->saturations=stats_.saturations;
        out->deferredRemovals=stats_.deferredRemovals;
        out->workConservingSelections=stats_.workConservingPicks;
    }
};
static_assert(sizeof(EevdfPolicy)<=SCHED_POLICY_STATE_CAPACITY,"EEVDF policy exceeds shared storage");
} // namespace brook
