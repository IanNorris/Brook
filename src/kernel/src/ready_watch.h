#pragma once
#include <stdint.h>

namespace brook {
inline constexpr unsigned ReadyWatchSlots=16;
inline constexpr unsigned ReadyWatchCpus=64;
enum class ReadyWatchTick { User, KernelBusy, Idle, Other };
struct ReadyWatchCpuActivity {
    uint32_t userTicks=0,kernelTicks=0,idleTicks=0,otherTicks=0;
    uint32_t userDispatches=0,kernelDispatches=0;
    bool Any()const{return userTicks || kernelTicks || idleTicks || otherTicks || userDispatches || kernelDispatches;}
    static bool Increment(uint32_t& n){if(n==~uint32_t(0))return true;++n;return false;}
};
struct ReadyWatchEvent {
    uint64_t readyNs=0,dispatchNs=0,deadlineTick=0,readyTick=0;
    uint64_t queueNs=0,lateMs=0,scoreNs=0;
    bool hasDeadline=false;
    ReadyWatchCpuActivity cpus[ReadyWatchCpus]{};
};
struct ReadyWatchSnapshot {
    uint64_t pid=0,incarnation=0,samples=0,timedSamples=0;
    uint64_t maxQueueNs=0,maxLateMs=0,slow=0,excluded=0,invalid=0;
    uint64_t invalidActivity=0;
    unsigned retained=0;
    bool saturated=false,pending=false;
    ReadyWatchEvent events[ReadyWatchSlots]{};
};
// Serialized by readyLock. Bounded diagnostics only; no Process pointers.
class ReadyWatchTracker {
    ReadyWatchSnapshot data_{};
    ReadyWatchEvent pending_{};
    void Add(uint64_t& n){if(n==~uint64_t(0))data_.saturated=true;else ++n;}
public:
    bool Matches(uint64_t pid,uint64_t incarnation)const {
        return data_.pid && data_.pid==pid && data_.incarnation==incarnation;
    }
    void Select(uint64_t pid,uint64_t incarnation){data_={};pending_={};data_.pid=pid;data_.incarnation=incarnation;}
    void Reset(){Select(data_.pid,data_.incarnation);}
    void Cancel(uint64_t pid,uint64_t incarnation){
        if(Matches(pid,incarnation)){pending_={};data_.pending=false;}
    }
    void Enqueue(uint64_t pid,uint64_t incarnation,uint64_t now){
        if(!Matches(pid,incarnation))return;
        pending_={};pending_.readyNs=now;data_.pending=true;
    }
    void Deadline(uint64_t pid,uint64_t incarnation,uint64_t deadline,uint64_t nowTick){
        if(!Matches(pid,incarnation) || !data_.pending || !deadline)return;
        pending_.deadlineTick=deadline;pending_.readyTick=nowTick;pending_.hasDeadline=true;
    }
    void Tick(unsigned cpu,ReadyWatchTick kind){
        if(!data_.pending)return;
        if(cpu>=ReadyWatchCpus){Add(data_.invalidActivity);return;}
        auto& c=pending_.cpus[cpu];uint32_t* counter=nullptr;
        switch(kind){
            case ReadyWatchTick::User:counter=&c.userTicks;break;
            case ReadyWatchTick::KernelBusy:counter=&c.kernelTicks;break;
            case ReadyWatchTick::Idle:counter=&c.idleTicks;break;
            case ReadyWatchTick::Other:counter=&c.otherTicks;break;
        }
        if(!counter){Add(data_.invalidActivity);return;}
        if(ReadyWatchCpuActivity::Increment(*counter))data_.saturated=true;
    }
    void OtherDispatch(unsigned cpu,uint64_t pid,uint64_t incarnation,bool kernel){
        if(!data_.pending || !pid || Matches(pid,incarnation))return;
        if(cpu>=ReadyWatchCpus){Add(data_.invalidActivity);return;}
        auto& c=pending_.cpus[cpu];
        if(ReadyWatchCpuActivity::Increment(kernel?c.kernelDispatches:c.userDispatches))data_.saturated=true;
    }
    void Dispatch(uint64_t pid,uint64_t incarnation,uint64_t now){
        if(!Matches(pid,incarnation) || !data_.pending)return;
        auto e=pending_;Cancel(pid,incarnation);
        if(!e.readyNs || !now || now<e.readyNs){Add(data_.invalid);return;}
        e.dispatchNs=now;e.queueNs=now-e.readyNs;
        if(e.hasDeadline){
            e.lateMs=e.readyTick>e.deadlineTick?e.readyTick-e.deadlineTick:0;
            Add(data_.timedSamples);
        }
        Add(data_.samples);
        if(e.queueNs>data_.maxQueueNs)data_.maxQueueNs=e.queueNs;
        if(e.lateMs>data_.maxLateMs)data_.maxLateMs=e.lateMs;
        e.scoreNs=e.lateMs>~uint64_t(0)/1000000?~uint64_t(0):e.lateMs*1000000;
        if(e.queueNs>e.scoreNs)e.scoreNs=e.queueNs;
        if(e.scoreNs<20000000)return;
        Add(data_.slow);unsigned at=0;
        while(at<data_.retained && data_.events[at].scoreNs>=e.scoreNs)++at;
        if(data_.retained==ReadyWatchSlots)Add(data_.excluded);
        if(at==ReadyWatchSlots)return;
        if(data_.retained<ReadyWatchSlots)++data_.retained;
        for(unsigned i=data_.retained-1;i>at;--i)data_.events[i]=data_.events[i-1];
        data_.events[at]=e;
    }
    ReadyWatchSnapshot Snapshot()const{return data_;}
};
}
