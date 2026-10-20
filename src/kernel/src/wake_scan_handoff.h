#pragma once
#include <stdint.h>
namespace brook {
struct WakeScanToken {uint64_t sequence=0,startNs=0,cpu=0;};
struct WakeScanSnapshot {
    uint64_t sequence=0,endSequence=0,shared=0,fallbackNs=0,lastCompletionNs=0;
    uint64_t bspScans=0,apScans=0,candidates=0,maxGapNs=0,maxDurationNs=0,invalidClock=0;
    uint64_t lastCpu=~uint64_t(0),saturated=0;
    bool consistent=false;
};
// One nonblocking claim covers gathering candidates, publishing their wakes,
// and dropping all temporary references. Never revoke an active owner. The
// sequence prevents stale completion/control tokens from releasing a new scan.
class WakeScanHandoff {
    uint64_t sequence_=0,shared_=0,lastCompletion_=0;
    uint64_t bsp_=0,ap_=0,candidates_=0,maxGap_=0,maxDuration_=0,invalid_=0,lastCpu_=~uint64_t(0),saturated_=0;
    const uint64_t fallbackNs_;
    static uint64_t Load(const uint64_t& v){return __atomic_load_n(&v,__ATOMIC_ACQUIRE);}
    static void Store(uint64_t& v,uint64_t x){__atomic_store_n(&v,x,__ATOMIC_RELEASE);}
    void Add(uint64_t& v,uint64_t n=1){
        const auto old=Load(v);if(n>~uint64_t(0)-old){Store(v,~uint64_t(0));Store(saturated_,1);}else Store(v,old+n);
    }
    uint64_t Claim(){
        auto old=Load(sequence_);if((old&1) || old>~uint64_t(0)-2)return 0;
        return __atomic_compare_exchange_n(&sequence_,&old,old+1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)?old+1:0;
    }
    bool Due(uint64_t now)const{
        const auto last=Load(lastCompletion_);
        return Load(shared_) && now && last && now>=last && now-last>=fallbackNs_;
    }
    void Release(uint64_t token){Store(sequence_,token+1);}
public:
    constexpr explicit WakeScanHandoff(uint64_t fallbackNs=2000000):fallbackNs_(fallbackNs){}
    bool Shared()const{return Load(shared_)!=0;}
    WakeScanToken Begin(unsigned cpu,uint64_t now){
        if(cpu && !Due(now))return {};
        const auto token=Claim();if(!token)return {};
        // The completion timestamp or mode may have changed after the first
        // check and before this claim. Recheck under exclusive ownership.
        if(cpu && !Due(now)){Release(token);return {};}
        return {token,now,cpu};
    }
    bool Complete(WakeScanToken token,uint64_t now,uint64_t candidates){
        if(!token.sequence || !(token.sequence&1) || Load(sequence_)!=token.sequence)return false;
        const auto last=Load(lastCompletion_);
        if(!now || (last && now<last) || (token.startNs && now<token.startNs)){
            Add(invalid_);Store(lastCompletion_,0);
        }else{
            if(last && now-last>Load(maxGap_))Store(maxGap_,now-last);
            if(token.startNs && now-token.startNs>Load(maxDuration_))Store(maxDuration_,now-token.startNs);
            Store(lastCompletion_,now);
        }
        Add(token.cpu?ap_:bsp_);Add(candidates_,candidates);Store(lastCpu_,token.cpu);
        Release(token.sequence);return true;
    }
    // mode -1 preserves the current mode. A busy owner yields a definite
    // rejection; controls do not wait, revoke a scan or mutate partial state.
    bool Control(int mode,bool reset){
        if(mode < -1 || mode > 1)return false;
        const auto token=Claim();if(!token)return false;
        if(mode>=0)Store(shared_,mode);
        if(reset){
            Store(lastCompletion_,0);Store(bsp_,0);Store(ap_,0);Store(candidates_,0);
            Store(maxGap_,0);Store(maxDuration_,0);Store(invalid_,0);Store(lastCpu_,~uint64_t(0));Store(saturated_,0);
        }
        Release(token);return true;
    }
    WakeScanSnapshot Snapshot()const{
        WakeScanSnapshot s;s.sequence=Load(sequence_);s.shared=Load(shared_);s.fallbackNs=fallbackNs_;
        s.lastCompletionNs=Load(lastCompletion_);s.bspScans=Load(bsp_);s.apScans=Load(ap_);
        s.candidates=Load(candidates_);s.maxGapNs=Load(maxGap_);s.maxDurationNs=Load(maxDuration_);
        s.invalidClock=Load(invalid_);s.lastCpu=Load(lastCpu_);s.saturated=Load(saturated_);s.endSequence=Load(sequence_);
        s.consistent=s.sequence==s.endSequence && !(s.sequence&1);return s;
    }
};
bool SchedulerWakeScanControl(int mode,bool reset);
WakeScanSnapshot SchedulerWakeScanRead();
}
