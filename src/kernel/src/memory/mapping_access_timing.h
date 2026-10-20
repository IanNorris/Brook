#pragma once
#include <stdint.h>

namespace brook {
// Diagnostic intervals only; caller serializes the reducer. No pointers or
// allocation, and no claim to cover other AS mutation-lock holders.
inline constexpr uint64_t MappingAccessTimingBounds[] = {
    100,500,1000,5000,10000,50000,100000,500000,1000000,5000000,
    10000000,50000000,100000000,500000000,1000000000,5000000000ULL
};
inline constexpr unsigned MappingAccessTimingBins=17;
struct MappingAccessTimingCounts {
    uint64_t samples=0,invalidClock=0,waitNs=0,holdNs=0,maxWaitNs=0,maxHoldNs=0;
    uint64_t maxWaitPid=0,maxWaitIncarnation=0,maxHoldPid=0,maxHoldIncarnation=0;
    uint64_t waitBins[MappingAccessTimingBins]{},holdBins[MappingAccessTimingBins]{};
    bool saturated=false;
};
struct MappingAccessTimingSnapshot {
    uint64_t epoch=0;
    MappingAccessTimingCounts read{},write{};
};
class MappingAccessTimingModel {
    MappingAccessTimingSnapshot state_{};
    bool exhausted_=false;
    static void Add(uint64_t& field,uint64_t value,bool& saturated) {
        if(field>~uint64_t(0)-value){field=~uint64_t(0);saturated=true;}
        else field+=value;
    }
    void Advance(bool enabled) {
        if(exhausted_ || state_.epoch>~uint64_t(0)-2){
            exhausted_=true;state_.epoch=0;return;
        }
        state_.epoch=((state_.epoch&~uint64_t(1))+2)|(enabled?1:0);
    }
public:
    uint64_t Token() const {return state_.epoch;}
    void Enable(bool enabled){Advance(enabled);}
    void Reset(){const bool enabled=(Token()&1)!=0;state_.read={};state_.write={};Advance(enabled);}
    MappingAccessTimingSnapshot Snapshot() const{return state_;}
    bool Observe(uint64_t token,bool write,uint64_t before,uint64_t acquired,uint64_t ended,
                 uint16_t pid,uint64_t incarnation) {
        if(!(token&1) || token!=Token())return false;
        auto& c=write?state_.write:state_.read;
        if(!before || acquired<before || ended<acquired){Add(c.invalidClock,1,c.saturated);return false;}
        const uint64_t wait=acquired-before,hold=ended-acquired;
        Add(c.samples,1,c.saturated);Add(c.waitNs,wait,c.saturated);Add(c.holdNs,hold,c.saturated);
        if(wait>c.maxWaitNs){c.maxWaitNs=wait;c.maxWaitPid=pid;c.maxWaitIncarnation=incarnation;}
        if(hold>c.maxHoldNs){c.maxHoldNs=hold;c.maxHoldPid=pid;c.maxHoldIncarnation=incarnation;}
        unsigned wb=0,hb=0;
        while(wb+1<MappingAccessTimingBins && wait>MappingAccessTimingBounds[wb])++wb;
        while(hb+1<MappingAccessTimingBins && hold>MappingAccessTimingBounds[hb])++hb;
        Add(c.waitBins[wb],1,c.saturated);Add(c.holdBins[hb],1,c.saturated);return true;
    }
};

extern uint64_t g_mappingAccessTimingEpoch;
inline uint64_t MappingAccessTimingToken(){return __atomic_load_n(&g_mappingAccessTimingEpoch,__ATOMIC_ACQUIRE);}
uint64_t MappingAccessTimingClock();
void MappingAccessTimingRecord(uint64_t token,bool write,uint64_t before,uint64_t acquired,
                              uint64_t ended,uint16_t pid,uint64_t incarnation);
void MappingAccessTimingEnable(bool enabled);
void MappingAccessTimingReset();
MappingAccessTimingSnapshot MappingAccessTimingRead();
}
