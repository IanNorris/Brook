#pragma once
#include <stdint.h>
namespace brook {
inline constexpr uint64_t AudioServicePeriodNs=10000000;
inline constexpr uint64_t AudioServiceSlowNs=20000000;
inline constexpr unsigned AudioServiceEventSlots=16;
struct AudioServiceEvent {
    uint64_t beforeSleepNs=0,afterDeadlineNs=0,wakeNs=0,endNs=0;
    uint64_t setupNs=0,lateLowerNs=0,serviceNs=0,tailNs=0,scoreNs=0;
    bool worked=false;
};
struct AudioServiceTimingSnapshot {
    uint64_t epoch=0,workerPid=0,workerIncarnation=0;
    uint64_t cycles=0,worked=0,invalidClock=0,sumSleepNs=0,maxSleepNs=0;
    uint64_t sumLateLowerNs=0,maxLateLowerNs=0,sumServiceNs=0,maxServiceNs=0;
    uint64_t maxSetupNs=0,maxTailNs=0,slowCycles=0,excludedSlow=0;
    uint64_t contendedRecords=0;
    uint32_t retained=0;
    bool saturated=false;
    AudioServiceEvent events[AudioServiceEventSlots]{};
};
// Caller serializes controls/snapshots/observations. No pointers/allocations.
// Holds the largest slow episodes, not a complete timeline.
class AudioServiceTimingModel {
    AudioServiceTimingSnapshot data_{};
    uint64_t lastEnd_=0;
    bool exhausted_=false;
    void Add(uint64_t& value,uint64_t add){
        if(value>~uint64_t(0)-add){value=~uint64_t(0);data_.saturated=true;}else value+=add;
    }
    void Advance(bool enabled){
        if(exhausted_ || data_.epoch>~uint64_t(0)-2){exhausted_=true;data_.epoch=0;return;}
        data_.epoch=((data_.epoch&~uint64_t(1))+2)|(enabled?1:0);lastEnd_=0;
    }
public:
    uint64_t Token() const{return data_.epoch;}
    void Worker(uint64_t pid,uint64_t incarnation){data_.workerPid=pid;data_.workerIncarnation=incarnation;}
    void Enable(bool enabled){Advance(enabled);}
    void Reset(){
        const auto epoch=data_.epoch,pid=data_.workerPid,inc=data_.workerIncarnation;
        data_={};data_.epoch=epoch;Worker(pid,inc);Advance((epoch&1)!=0);
    }
    AudioServiceTimingSnapshot Snapshot()const{return data_;}
    bool Observe(uint64_t token,uint64_t before,uint64_t deadline,uint64_t wake,uint64_t end,bool worked){
        if(!(token&1) || token!=Token())return false;
        if(!before || deadline<before || wake<deadline || end<wake ||
           deadline>~uint64_t(0)-AudioServicePeriodNs || (lastEnd_ && before<lastEnd_)){
            Add(data_.invalidClock,1);lastEnd_=0;return false;
        }
        const uint64_t dueUpper=deadline+AudioServicePeriodNs;
        AudioServiceEvent e{before,deadline,wake,end,deadline-before,
            wake>dueUpper?wake-dueUpper:0,end-wake,lastEnd_?before-lastEnd_:0,0,worked};
        lastEnd_=end;
        const uint64_t sleep=wake-deadline;
        Add(data_.cycles,1);if(worked)Add(data_.worked,1);
        Add(data_.sumSleepNs,sleep);Add(data_.sumLateLowerNs,e.lateLowerNs);Add(data_.sumServiceNs,e.serviceNs);
        if(sleep>data_.maxSleepNs)data_.maxSleepNs=sleep;
        if(e.lateLowerNs>data_.maxLateLowerNs)data_.maxLateLowerNs=e.lateLowerNs;
        if(e.serviceNs>data_.maxServiceNs)data_.maxServiceNs=e.serviceNs;
        if(e.setupNs>data_.maxSetupNs)data_.maxSetupNs=e.setupNs;
        if(e.tailNs>data_.maxTailNs)data_.maxTailNs=e.tailNs;
        e.scoreNs=e.setupNs;
        if(e.lateLowerNs>e.scoreNs)e.scoreNs=e.lateLowerNs;
        if(e.serviceNs>e.scoreNs)e.scoreNs=e.serviceNs;
        if(e.tailNs>e.scoreNs)e.scoreNs=e.tailNs;
        if(e.scoreNs<AudioServiceSlowNs)return true;
        Add(data_.slowCycles,1);unsigned at=0;
        while(at<data_.retained && data_.events[at].scoreNs>=e.scoreNs)++at;
        if(data_.retained==AudioServiceEventSlots)Add(data_.excludedSlow,1);
        if(at==AudioServiceEventSlots)return true;
        if(data_.retained<AudioServiceEventSlots)++data_.retained;
        for(unsigned i=data_.retained-1;i>at;--i)data_.events[i]=data_.events[i-1];
        data_.events[at]=e;return true;
    }
};
void AudioServiceTimingEnable(bool enabled);
void AudioServiceTimingReset();
AudioServiceTimingSnapshot AudioServiceTimingRead();
}
