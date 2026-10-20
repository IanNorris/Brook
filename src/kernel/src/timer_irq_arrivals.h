#pragma once
#include <stdint.h>

namespace brook {
inline constexpr unsigned TimerIrqHandlerSlots=16;
struct TimerIrqHandlerToken {
    uint64_t epoch=0,entryNs=0,rip=0,cs=0;
    unsigned cpu=0;
    uint64_t switchGeneration=~uint64_t(0),sequence=0,previousEndNs=0;
};
struct TimerIrqHandlerEvent {
    TimerIrqHandlerToken token{};
    uint64_t schedulerNs=0,endNs=0,prefixNs=0,schedulerWallNs=0,totalNs=0;
    uint64_t completionGeneration=~uint64_t(0);
    bool noSwitch=false;
};
struct TimerIrqArrivalSnapshot {
    uint64_t epoch=0,arrivals=0,gaps=0,invalidClock=0;
    uint64_t firstNs=0,lastNs=0,maxGapNs=0,maxGapStartNs=0,maxGapEndNs=0;
    uint64_t maxGapRip=0,maxGapCs=0,slow=0,contended=0;
    uint64_t handlerCompleted=0,handlerInvalidClock=0,handlerMigrated=0;
    uint64_t maxHandlerPrefixNs=0,maxHandlerSchedulerNs=0,maxHandlerTotalNs=0;
    uint64_t handlerSlow=0,handlerExcluded=0;
    uint64_t sumHandlerPrefixNs=0,sumHandlerSchedulerNs=0,sumHandlerTotalNs=0;
    uint64_t handlerNoSwitch=0,handlerSwitched=0,handlerUnknownGeneration=0;
    uint64_t sumNoSwitchTotalNs=0,maxNoSwitchTotalNs=0,maxNoSwitchSchedulerNs=0;
    uint64_t adjacentNoSwitchPairs=0,nearReturns=0,sumBetweenNs=0,maxBetweenNs=0;
    unsigned handlerRetained=0;
    TimerIrqHandlerEvent maxPrefixEvent{},maxSchedulerEvent{};
    TimerIrqHandlerEvent handlerEvents[TimerIrqHandlerSlots]{};
    bool saturated=false;
};

// One independently serialized model per CPU. An unavailable/regressing clock
// breaks adjacency: the next valid observation cannot hide an unmeasured span.
class TimerIrqArrivalModel {
    TimerIrqArrivalSnapshot data_{};
    uint64_t lastCompletionSequence_=0,lastCompletionEnd_=0,lastCompletionGeneration_=~uint64_t(0);
    bool lastCompletionNoSwitch_=false;
    void Add(uint64_t& n){if(n==~uint64_t(0))data_.saturated=true;else ++n;}
    void Sum(uint64_t& n,uint64_t v){if(v>~uint64_t(0)-n){n=~uint64_t(0);data_.saturated=true;}else n+=v;}
public:
    void Reset(uint64_t epoch){data_={};data_.epoch=epoch;
        lastCompletionSequence_=lastCompletionEnd_=0;lastCompletionGeneration_=~uint64_t(0);lastCompletionNoSwitch_=false;
    }
    void SetEpoch(uint64_t epoch){data_.epoch=epoch;}
    TimerIrqHandlerToken Observe(uint64_t epoch,uint64_t now,uint64_t rip,uint64_t cs,unsigned cpu=0,uint64_t generation=~uint64_t(0)){
        if(!(epoch&1) || epoch!=data_.epoch)return {};
        if(!now || (data_.lastNs && now<data_.lastNs)){
            Add(data_.invalidClock);data_.lastNs=0;lastCompletionNoSwitch_=false;return {};
        }
        Add(data_.arrivals);
        if(!data_.firstNs)data_.firstNs=now;
        if(data_.lastNs){
            const auto gap=now-data_.lastNs;Add(data_.gaps);
            if(gap>=20000000)Add(data_.slow);
            if(gap>data_.maxGapNs){
                data_.maxGapNs=gap;data_.maxGapStartNs=data_.lastNs;
                data_.maxGapEndNs=now;data_.maxGapRip=rip;data_.maxGapCs=cs;
            }
        }
        data_.lastNs=now;
        TimerIrqHandlerToken token{epoch,now,rip,cs,cpu,generation,data_.arrivals,0};
        if(lastCompletionNoSwitch_ && lastCompletionSequence_ && data_.arrivals-lastCompletionSequence_==1 &&
           generation!=~uint64_t(0) && generation==lastCompletionGeneration_ && now>=lastCompletionEnd_)
            token.previousEndNs=lastCompletionEnd_;
        return token;
    }
    bool Complete(const TimerIrqHandlerToken& t,uint64_t scheduler,uint64_t end,unsigned cpu,uint64_t generation=~uint64_t(0)){
        if(!(t.epoch&1) || t.epoch!=data_.epoch)return false;
        if(cpu!=t.cpu){Add(data_.handlerMigrated);lastCompletionNoSwitch_=false;return false;}
        if(!t.entryNs || scheduler<t.entryNs || end<scheduler){Add(data_.handlerInvalidClock);lastCompletionNoSwitch_=false;return false;}
        TimerIrqHandlerEvent e{t,scheduler,end,scheduler-t.entryNs,end-scheduler,end-t.entryNs,generation,false};
        const bool known=t.switchGeneration!=~uint64_t(0) && generation!=~uint64_t(0);
        e.noSwitch=known && t.switchGeneration==generation;
        lastCompletionSequence_=t.sequence;lastCompletionEnd_=end;lastCompletionGeneration_=generation;lastCompletionNoSwitch_=e.noSwitch;
        if(!known)Add(data_.handlerUnknownGeneration);
        else if(!e.noSwitch)Add(data_.handlerSwitched);
        else {
            Add(data_.handlerNoSwitch);Sum(data_.sumNoSwitchTotalNs,e.totalNs);
            if(e.totalNs>data_.maxNoSwitchTotalNs)data_.maxNoSwitchTotalNs=e.totalNs;
            if(e.schedulerWallNs>data_.maxNoSwitchSchedulerNs)data_.maxNoSwitchSchedulerNs=e.schedulerWallNs;
            if(t.previousEndNs){
                const auto between=t.entryNs-t.previousEndNs;Add(data_.adjacentNoSwitchPairs);Sum(data_.sumBetweenNs,between);
                if(between<=100000)Add(data_.nearReturns);
                if(between>data_.maxBetweenNs)data_.maxBetweenNs=between;
            }
        }
        Add(data_.handlerCompleted);
        Sum(data_.sumHandlerPrefixNs,e.prefixNs);Sum(data_.sumHandlerSchedulerNs,e.schedulerWallNs);Sum(data_.sumHandlerTotalNs,e.totalNs);
        if(e.prefixNs>data_.maxHandlerPrefixNs){data_.maxHandlerPrefixNs=e.prefixNs;data_.maxPrefixEvent=e;}
        if(e.schedulerWallNs>data_.maxHandlerSchedulerNs){data_.maxHandlerSchedulerNs=e.schedulerWallNs;data_.maxSchedulerEvent=e;}
        if(e.totalNs>data_.maxHandlerTotalNs)data_.maxHandlerTotalNs=e.totalNs;
        // Largest 16 >=1ms no-switch C-handler wall brackets. Host delay,
        // nested IRQs, and time outside the C body are not CPU residency proof.
        if(!e.noSwitch || e.totalNs<1000000)return true;
        Add(data_.handlerSlow);unsigned at=0;
        while(at<data_.handlerRetained && data_.handlerEvents[at].totalNs>=e.totalNs)++at;
        if(data_.handlerRetained==TimerIrqHandlerSlots)Add(data_.handlerExcluded);
        if(at==TimerIrqHandlerSlots)return true;
        if(data_.handlerRetained<TimerIrqHandlerSlots)++data_.handlerRetained;
        for(unsigned i=data_.handlerRetained-1;i>at;--i)data_.handlerEvents[i]=data_.handlerEvents[i-1];
        data_.handlerEvents[at]=e;return true;
    }
    TimerIrqArrivalSnapshot Snapshot()const{return data_;}
};

// Default off; separate from the more expensive BSP wake-scan probe. Controls
// reset an epoch, IRQ observations try-lock only, reports expose mixed epochs.
void TimerIrqArrivalsEnable(bool enabled);
void TimerIrqArrivalsReset();
uint64_t TimerIrqArrivalsEpoch();
TimerIrqArrivalSnapshot TimerIrqArrivalsRead(unsigned cpu);
unsigned TimerIrqArrivalsCpuCapacity();
}
