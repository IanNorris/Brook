#pragma once
#include <stdint.h>
#include "runtime_accounting.h"
namespace brook {
enum class MlfqDecisionReason:uint8_t {Service=1,LegacyWall=2,HigherPriority=3};
struct SchedDecisionEvent {
    uint64_t epoch=0,pid=0,incarnation=0,nowNs=0,rip=0,generation=0;
    uint64_t usedNs=0,quantumNs=0;
    RuntimeSample dispatch{},drained{};
    uint32_t cpu=0;
    uint8_t priority=0,higherPriority=255;
    MlfqDecisionReason reason=MlfqDecisionReason::Service;
    bool serviceSeen=false,hasDispatch=false,hasDrain=false,hasSteal=false,periodic=false;
};
struct SchedDecisionSnapshot {
    uint64_t epoch=0,pid=0,incarnation=0,observations=0,serviceExpiry=0,wallExpiry=0,higherPriority=0;
    uint64_t invalid=0,unpaired=0,contended=0,excluded=0,maxUsedNs=0;
    bool saturated=false;
    unsigned retained=0;
    SchedDecisionEvent events[32]{};
};
// Independently keyed observations, no continuity inferred across drops.
// The scheduler serializes model access through bounded IRQ-masked try sections.
class SchedDecisionModel {
    SchedDecisionSnapshot data_{};bool exhausted_=false;
    void Add(uint64_t& value){if(value==~uint64_t(0))data_.saturated=true;else ++value;}
    void Advance(bool enabled){
        if(exhausted_ || data_.epoch>~uint64_t(0)-2){exhausted_=true;data_.epoch=0;return;}
        data_.epoch=((data_.epoch&~uint64_t(1))+2)|(enabled?1:0);
    }
public:
    SchedDecisionSnapshot Snapshot()const{return data_;}
    void Watch(uint64_t pid,uint64_t incarnation){auto epoch=data_.epoch;data_={};data_.epoch=epoch;data_.pid=pid;data_.incarnation=incarnation;Advance(pid && incarnation);}
    void Off(){Advance(false);}
    void Reset(){auto old=data_;data_={};data_.epoch=old.epoch;data_.pid=old.pid;data_.incarnation=old.incarnation;Advance(old.epoch&1);}
    bool Observe(const SchedDecisionEvent& e){
        if(!(e.epoch&1) || e.epoch!=data_.epoch || e.pid!=data_.pid || e.incarnation!=data_.incarnation)return false;
        bool valid=e.nowNs && e.priority<4 && e.quantumNs && e.generation!=~uint64_t(0);
        if(e.generation==~uint64_t(0))data_.saturated=true;
        switch(e.reason){
            case MlfqDecisionReason::Service:valid=valid && e.serviceSeen && e.usedNs>=e.quantumNs;break;
            case MlfqDecisionReason::LegacyWall:valid=valid && !e.serviceSeen;break;
            case MlfqDecisionReason::HigherPriority:valid=valid && e.higherPriority<e.priority && (!e.serviceSeen || e.usedNs<e.quantumNs);break;
            default:valid=false;
        }
        if(e.hasDispatch)valid=valid && e.dispatch.valid && e.dispatch.wallNs && e.dispatch.wallNs<=e.nowNs;
        if(e.hasDrain)valid=valid && e.hasDispatch && e.drained.valid && e.drained.wallNs>=e.dispatch.wallNs && e.drained.wallNs<=e.nowNs && e.drained.stealNs>=e.dispatch.stealNs;
        if(!valid){Add(data_.invalid);return false;}
        Add(data_.observations);if(!e.hasDispatch || !e.hasDrain)Add(data_.unpaired);
        if(e.reason==MlfqDecisionReason::Service)Add(data_.serviceExpiry);
        else if(e.reason==MlfqDecisionReason::LegacyWall)Add(data_.wallExpiry);
        else Add(data_.higherPriority);
        if(e.usedNs>data_.maxUsedNs)data_.maxUsedNs=e.usedNs;
        // Largest observed charged budgets; ties keep earlier observations.
        unsigned at=0;while(at<data_.retained && data_.events[at].usedNs>=e.usedNs)++at;
        if(data_.retained==32)Add(data_.excluded);if(at==32)return true;
        if(data_.retained<32)++data_.retained;
        for(unsigned i=data_.retained-1;i>at;--i)data_.events[i]=data_.events[i-1];data_.events[at]=e;return true;
    }
};
bool SchedDecisionWatch(uint64_t pid,uint64_t incarnation,SchedDecisionSnapshot* output);
bool SchedDecisionOff(SchedDecisionSnapshot* output);
bool SchedDecisionReset(SchedDecisionSnapshot* output);
bool SchedDecisionRead(SchedDecisionSnapshot* output);
uint64_t SchedDecisionToken(uint64_t pid,uint64_t incarnation);
void SchedDecisionRecord(const SchedDecisionEvent& event);
}
// Optional observation import, not a policy ABI/layout change. Native MLFQ
// requires the matched kernel export; hosted tests provide an observation stub.
extern "C" void SchedulerObserveMlfqDecision(uint16_t pid,uint8_t priority,uint64_t usedNs,
    uint64_t quantumNs,uint8_t reason,bool serviceSeen,uint8_t higherPriority);
