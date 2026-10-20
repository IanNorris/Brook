#pragma once
#include <stdint.h>
namespace brook {
inline constexpr unsigned TimerWakeSlots=16;
struct TimerWakeToken {
    uint64_t epoch=0,sequence=0,arrivalNs=0,previousArrivalNs=0,gapNs=0;
    uint64_t rip=0,cs=0;
};
struct TimerWakeEvent {
    TimerWakeToken token{};
    uint64_t schedulerNs=0,afterRuntimeNs=0,beforeScanNs=0,afterScanNs=0;
    uint64_t prefixNs=0,runtimeNs=0,preScanNs=0,scanNs=0,scoreNs=0;
    uint32_t candidates=0;
};
struct TimerWakeSnapshot {
    uint64_t epoch=0,arrivals=0,completed=0,invalidClock=0;
    uint64_t maxGapNs=0,maxPrefixNs=0,maxRuntimeNs=0,maxPreScanNs=0,maxScanNs=0;
    uint64_t slow=0,excluded=0,contendedEntries=0,contendedRecords=0;
    unsigned retained=0;
    bool saturated=false;
    TimerWakeEvent events[TimerWakeSlots]{};
};
// Caller serializes all access. IRQ collection must use try-lock acquisition.
class TimerWakeTimingModel {
    TimerWakeSnapshot data_{};
    uint64_t lastArrival_=0;
    bool exhausted_=false;
    void Add(uint64_t& n){if(n==~uint64_t(0))data_.saturated=true;else ++n;}
    void Advance(bool enabled){
        if(exhausted_ || data_.epoch>~uint64_t(0)-2){exhausted_=true;data_.epoch=0;return;}
        data_.epoch=((data_.epoch&~uint64_t(1))+2)|(enabled?1:0);lastArrival_=0;
    }
public:
    uint64_t Epoch()const{return data_.epoch;}
    void Enable(bool enabled){Advance(enabled);}
    void Reset(){const auto epoch=data_.epoch;data_={};data_.epoch=epoch;Advance(epoch&1);}
    TimerWakeToken Begin(uint64_t epoch,uint64_t now,uint64_t rip,uint64_t cs){
        if(!(epoch&1) || epoch!=data_.epoch)return {};
        if(!now || (lastArrival_ && now<lastArrival_)){Add(data_.invalidClock);lastArrival_=0;return {};}
        Add(data_.arrivals);
        TimerWakeToken t{epoch,data_.arrivals,now,lastArrival_,lastArrival_?now-lastArrival_:0,rip,cs};
        lastArrival_=now;if(t.gapNs>data_.maxGapNs)data_.maxGapNs=t.gapNs;return t;
    }
    bool Complete(const TimerWakeToken& t,uint64_t scheduler,uint64_t runtime,
                  uint64_t scan,uint64_t end,uint32_t candidates){
        if(!(t.epoch&1) || t.epoch!=data_.epoch)return false;
        if(!t.arrivalNs || scheduler<t.arrivalNs || runtime<scheduler || scan<runtime || end<scan){
            Add(data_.invalidClock);return false;
        }
        TimerWakeEvent e{t,scheduler,runtime,scan,end,scheduler-t.arrivalNs,
            runtime-scheduler,scan-runtime,end-scan,0,candidates};
        Add(data_.completed);
        if(e.prefixNs>data_.maxPrefixNs)data_.maxPrefixNs=e.prefixNs;
        if(e.runtimeNs>data_.maxRuntimeNs)data_.maxRuntimeNs=e.runtimeNs;
        if(e.preScanNs>data_.maxPreScanNs)data_.maxPreScanNs=e.preScanNs;
        if(e.scanNs>data_.maxScanNs)data_.maxScanNs=e.scanNs;
        e.scoreNs=t.gapNs;
        const uint64_t parts[]={e.prefixNs,e.runtimeNs,e.preScanNs,e.scanNs};
        for(auto n:parts)if(n>e.scoreNs)e.scoreNs=n;
        if(e.scoreNs<20000000)return true;
        Add(data_.slow);unsigned at=0;
        while(at<data_.retained && data_.events[at].scoreNs>=e.scoreNs)++at;
        if(data_.retained==TimerWakeSlots)Add(data_.excluded);
        if(at==TimerWakeSlots)return true;
        if(data_.retained<TimerWakeSlots)++data_.retained;
        for(unsigned i=data_.retained-1;i>at;--i)data_.events[i]=data_.events[i-1];
        data_.events[at]=e;return true;
    }
    TimerWakeSnapshot Snapshot()const{return data_;}
};
// Normal-thread controls; entry/record are nonblocking BSP IRQ probes.
void TimerWakeTimingEnable(bool enabled);
void TimerWakeTimingReset();
TimerWakeSnapshot TimerWakeTimingRead();
TimerWakeToken TimerWakeTimingBegin(uint64_t rip,uint64_t cs);
uint64_t TimerWakeTimingNow();
void TimerWakeTimingComplete(const TimerWakeToken& t,uint64_t scheduler,uint64_t runtime,
                             uint64_t scan,uint64_t end,uint32_t candidates);
}
