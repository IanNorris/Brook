#pragma once
#include <stdint.h>

namespace brook {
struct TimerIrqArrivalSnapshot {
    uint64_t epoch=0,arrivals=0,gaps=0,invalidClock=0;
    uint64_t firstNs=0,lastNs=0,maxGapNs=0,maxGapStartNs=0,maxGapEndNs=0;
    uint64_t maxGapRip=0,maxGapCs=0,slow=0,contended=0;
    bool saturated=false;
};

// One independently serialized model per CPU. An unavailable/regressing clock
// breaks adjacency: the next valid observation cannot hide an unmeasured span.
class TimerIrqArrivalModel {
    TimerIrqArrivalSnapshot data_{};
    void Add(uint64_t& n){if(n==~uint64_t(0))data_.saturated=true;else ++n;}
public:
    void Reset(uint64_t epoch){data_={};data_.epoch=epoch;}
    void SetEpoch(uint64_t epoch){data_.epoch=epoch;}
    void Observe(uint64_t epoch,uint64_t now,uint64_t rip,uint64_t cs){
        if(!(epoch&1) || epoch!=data_.epoch)return;
        if(!now || (data_.lastNs && now<data_.lastNs)){
            Add(data_.invalidClock);data_.lastNs=0;return;
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
