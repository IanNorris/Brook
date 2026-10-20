#pragma once
#include <stdint.h>

namespace brook {
// Completed DSP write brackets only. Wall time includes scheduling and host
// descheduling; neither mutex nor backpressure brackets are CPU-time measures.
struct DspProducerEvent {
    uint64_t pid=0,incarnation=0,stream=0,beginNs=0,endNs=0,requested=0;
    uint64_t mutexNs=0,backpressureNs=0,attempts=0,sleeps=0,gapNs=0;
    int64_t result=0;
    bool valid=true,hasResult=false,hasGap=false;
};
struct DspProducerSnapshot {
    uint64_t epoch=0,completed=0,errors=0,acceptedBytes=0,invalid=0,overflowWriters=0;
    uint64_t sumCallNs=0,sumMutexNs=0,sumBackpressureNs=0;
    uint64_t maxCallNs=0,maxMutexNs=0,maxBackpressureNs=0,maxGapNs=0;
    uint64_t slow=0,excluded=0,contended=0;
    bool saturated=false;
    unsigned retained=0,writers=0;
    DspProducerEvent events[16]{};
};
class DspProducerTimingModel {
    struct Writer {uint64_t pid=0,incarnation=0,stream=0,lastEnd=0;} writers_[8]{};
    DspProducerSnapshot data_{};
    bool exhausted_=false;
    void Add(uint64_t& value,uint64_t delta=1){
        if(value>~uint64_t(0)-delta){value=~uint64_t(0);data_.saturated=true;}else value+=delta;
    }
    void Advance(bool enabled){
        for(auto& w:writers_)w.lastEnd=0;
        if(exhausted_ || data_.epoch>~uint64_t(0)-2){exhausted_=true;data_.epoch=0;return;}
        data_.epoch=((data_.epoch&~uint64_t(1))+2)|(enabled?1:0);
    }
    static uint64_t Score(const DspProducerEvent& e){
        const auto duration=e.endNs-e.beginNs;return duration>e.gapNs?duration:e.gapNs;
    }
public:
    uint64_t Token()const{return data_.epoch;}
    void Enable(bool on){Advance(on);}
    void Reset(){const auto epoch=data_.epoch;data_={};for(auto& w:writers_)w={};data_.epoch=epoch;Advance(epoch&1);}
    DspProducerSnapshot Snapshot()const{return data_;}
    bool Observe(uint64_t token,DspProducerEvent e){
        if(!(token&1) || token!=Token())return false;
        if(!e.pid || !e.incarnation || !e.stream){Add(data_.invalid);return false;}
        Writer* writer=nullptr;
        for(unsigned i=0;i<data_.writers;++i){auto& w=writers_[i];
            if(w.pid==e.pid && w.incarnation==e.incarnation && w.stream==e.stream){writer=&w;break;}}
        if(!writer){
            if(data_.writers==8){Add(data_.overflowWriters);return false;}
            writer=&writers_[data_.writers++];*writer={e.pid,e.incarnation,e.stream,0};
        }
        if(!e.valid || !e.hasResult || !e.pid || !e.incarnation || !e.stream || !e.beginNs || e.endNs<e.beginNs ||
           e.mutexNs>e.endNs-e.beginNs || e.backpressureNs>e.endNs-e.beginNs-e.mutexNs ||
           (writer->lastEnd && e.beginNs<writer->lastEnd) ||
           (e.result>0 && uint64_t(e.result)>e.requested)){
            Add(data_.invalid);writer->lastEnd=0;return false;
        }
        e.hasGap=writer->lastEnd!=0;e.gapNs=e.hasGap?e.beginNs-writer->lastEnd:0;writer->lastEnd=e.endNs;
        const auto duration=e.endNs-e.beginNs;
        Add(data_.completed);if(e.result<0)Add(data_.errors);if(e.result>0)Add(data_.acceptedBytes,uint64_t(e.result));
        Add(data_.sumCallNs,duration);Add(data_.sumMutexNs,e.mutexNs);Add(data_.sumBackpressureNs,e.backpressureNs);
        if(duration>data_.maxCallNs)data_.maxCallNs=duration;
        if(e.mutexNs>data_.maxMutexNs)data_.maxMutexNs=e.mutexNs;
        if(e.backpressureNs>data_.maxBackpressureNs)data_.maxBackpressureNs=e.backpressureNs;
        if(e.gapNs>data_.maxGapNs)data_.maxGapNs=e.gapNs;
        if(Score(e)<20000000)return true;
        Add(data_.slow);unsigned at=0;while(at<data_.retained && Score(data_.events[at])>=Score(e))++at;
        if(data_.retained==16)Add(data_.excluded);
        if(at==16)return true;
        if(data_.retained<16)++data_.retained;
        for(unsigned i=data_.retained-1;i>at;--i)data_.events[i]=data_.events[i-1];data_.events[at]=e;
        return true;
    }
};
void DspProducerTimingEnable(bool enabled);
void DspProducerTimingReset();
DspProducerSnapshot DspProducerTimingRead();
uint64_t DspProducerTimingToken();
void DspProducerTimingRecord(uint64_t token,const DspProducerEvent& event);
extern "C" uint64_t AudioClockNowNs();
class DspProducerCall {
    uint64_t token_=0,lastNs_=0;
    DspProducerEvent event_{};
    void Span(uint64_t begin,uint64_t end,uint64_t& sum){
        if(!begin || end<begin || sum>~uint64_t(0)-(end-begin)){event_.valid=false;return;}sum+=end-begin;
    }
public:
    DspProducerCall(uint64_t pid,uint64_t incarnation,uint64_t stream,uint64_t requested):token_(DspProducerTimingToken()){
        if(token_&1){event_.pid=pid;event_.incarnation=incarnation;event_.stream=stream;event_.requested=requested;event_.beginNs=Mark();}
    }
    uint64_t Mark(){
        if(!(token_&1))return 0;
        const auto now=AudioClockNowNs();if(!now || now<lastNs_)event_.valid=false;lastNs_=now;return now;
    }
    void Mutex(uint64_t before){if(token_&1){Span(before,Mark(),event_.mutexNs);if(event_.attempts==~uint64_t(0))event_.valid=false;else ++event_.attempts;}}
    void Sleep(uint64_t before){if(token_&1){Span(before,Mark(),event_.backpressureNs);if(event_.sleeps==~uint64_t(0))event_.valid=false;else ++event_.sleeps;}}
    int64_t Result(int64_t result){event_.result=result;event_.hasResult=true;return result;}
    ~DspProducerCall(){if(token_&1){event_.endNs=Mark();DspProducerTimingRecord(token_,event_);}}
};
}
