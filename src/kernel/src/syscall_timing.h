#pragma once
#include <stdint.h>
namespace brook {
enum class SyscallTimingPhase : uint8_t { Dispatch,ReturnCheck };
struct SyscallTimingTicket {uint64_t epoch=0,sequence=0;};
struct SyscallTimingEvent {uint64_t number=0,arg0=0,beginNs=0,endNs=0,gapNs=0;int64_t result=0;bool hasGap=false;SyscallTimingPhase phase=SyscallTimingPhase::Dispatch;};
struct SyscallTimingSnapshot {
    uint64_t epoch=0,pid=0,incarnation=0,entries=0,completed=0,invalid=0,incomplete=0,overflowKinds=0;
    uint64_t contended=0,continuityBreaks=0,maxCallNs=0,maxGapNs=0,slow=0,excluded=0;
    bool active=false,saturated=false;
    uint64_t sequence=0,activeNumber=0,activeArg0=0,activeBeginNs=0;
    SyscallTimingPhase activePhase=SyscallTimingPhase::Dispatch;
    unsigned kinds=0,retained=0;
    struct Kind {uint64_t number=0,calls=0,sumNs=0,maxNs=0,negativeResults=0;SyscallTimingPhase phase=SyscallTimingPhase::Dispatch;} kind[16]{};
    SyscallTimingEvent events[16]{};
};
// Caller serializes observations; one explicitly selected thread incarnation.
// Unmatched entry is not proof of liveness: exit or a dropped completion may
// leave it unmatched. Gaps cover only adjacent recorded phase boundaries;
// they include uninstrumented kernel, user and descheduled time.
class SyscallTimingModel {
    SyscallTimingSnapshot data_{};
    uint64_t lastEnd_=0,activeGap_=0;
    bool activeHasGap_=false,exhausted_=false;
    void Add(uint64_t& n,uint64_t delta=1){if(n>~uint64_t(0)-delta){n=~uint64_t(0);data_.saturated=true;}else n+=delta;}
    void Advance(bool on){
        lastEnd_=0;data_.active=false;
        if(exhausted_ || data_.epoch>~uint64_t(0)-2){exhausted_=true;data_.epoch=0;return;}
        data_.epoch=((data_.epoch&~uint64_t(1))+2)|(on?1:0);
    }
    static uint64_t Score(const SyscallTimingEvent& e){auto call=e.endNs-e.beginNs;return call>e.gapNs?call:e.gapNs;}
public:
    uint64_t Token()const{return data_.epoch;}
    bool Matches(uint64_t pid,uint64_t incarnation)const{return (data_.epoch&1) && data_.pid==pid && data_.incarnation==incarnation;}
    SyscallTimingSnapshot Snapshot()const{return data_;}
    void Watch(uint64_t pid,uint64_t incarnation){
        auto epoch=data_.epoch;data_={};data_.epoch=epoch;data_.pid=pid;data_.incarnation=incarnation;Advance(pid && incarnation);
    }
    void BreakContinuity(bool exhausted=false){
        lastEnd_=0;activeGap_=0;activeHasGap_=false;Add(data_.continuityBreaks);if(exhausted)data_.saturated=true;
    }
    void Off(){Advance(false);}
    void Reset(){auto old=data_;data_={};data_.epoch=old.epoch;data_.pid=old.pid;data_.incarnation=old.incarnation;Advance(old.epoch&1);}
    SyscallTimingTicket Begin(uint64_t token,uint64_t pid,uint64_t incarnation,uint64_t number,uint64_t arg0,uint64_t now,SyscallTimingPhase phase=SyscallTimingPhase::Dispatch){
        if(!(token&1) || token!=Token() || pid!=data_.pid || incarnation!=data_.incarnation)return {};
        if(data_.active){
            const auto previous=data_.activeBeginNs;Add(data_.incomplete);lastEnd_=0;data_.active=false;
            if(now<previous){Add(data_.invalid);return {};}
        }
        if(!now || (lastEnd_ && now<lastEnd_) || data_.sequence==~uint64_t(0)){
            Add(data_.invalid);if(data_.sequence==~uint64_t(0))data_.saturated=true;lastEnd_=0;return {};
        }
        activeHasGap_=lastEnd_!=0;activeGap_=lastEnd_?now-lastEnd_:0;
        data_.active=true;data_.activePhase=phase;data_.activeNumber=number;data_.activeArg0=arg0;data_.activeBeginNs=now;
        ++data_.sequence;Add(data_.entries);return {token,data_.sequence};
    }
    bool End(SyscallTimingTicket ticket,uint64_t now,int64_t result,bool hasResult=true){
        if(!(ticket.epoch&1) || ticket.epoch!=Token() || !data_.active || ticket.sequence!=data_.sequence)return false;
        data_.active=false;
        if(!hasResult || now<data_.activeBeginNs){Add(data_.invalid);lastEnd_=0;return false;}
        SyscallTimingEvent e{data_.activeNumber,data_.activeArg0,data_.activeBeginNs,now,activeGap_,result,activeHasGap_,data_.activePhase};
        lastEnd_=now;Add(data_.completed);const auto duration=now-e.beginNs;
        if(duration>data_.maxCallNs)data_.maxCallNs=duration;if(e.gapNs>data_.maxGapNs)data_.maxGapNs=e.gapNs;
        unsigned at=0;while(at<data_.kinds && (data_.kind[at].number!=e.number || data_.kind[at].phase!=e.phase))++at;
        if(at==data_.kinds && at<16){data_.kind[at].number=e.number;data_.kind[at].phase=e.phase;++data_.kinds;}
        if(at==16)Add(data_.overflowKinds);
        else {auto& k=data_.kind[at];Add(k.calls);Add(k.sumNs,duration);if(duration>k.maxNs)k.maxNs=duration;if(result<0)Add(k.negativeResults);}
        if(Score(e)<20000000)return true;
        Add(data_.slow);at=0;while(at<data_.retained && Score(data_.events[at])>=Score(e))++at;
        if(data_.retained==16)Add(data_.excluded);if(at==16)return true;
        if(data_.retained<16)++data_.retained;
        for(unsigned i=data_.retained-1;i>at;--i)data_.events[i]=data_.events[i-1];data_.events[at]=e;return true;
    }
};
// Controls/readers and recorders use bounded try-locks. False means no control
// effect or no snapshot; callers may retry explicit busy responses.
bool SyscallTimingWatch(uint64_t pid,uint64_t incarnation,SyscallTimingSnapshot* output=nullptr);
bool SyscallTimingOff(SyscallTimingSnapshot* output=nullptr);
bool SyscallTimingReset(SyscallTimingSnapshot* output=nullptr);
bool SyscallTimingRead(SyscallTimingSnapshot* output);
SyscallTimingTicket SyscallTimingEnter(uint64_t pid,uint64_t incarnation,uint64_t number,uint64_t arg0,SyscallTimingPhase phase=SyscallTimingPhase::Dispatch);
void SyscallTimingLeave(SyscallTimingTicket ticket,int64_t result,bool hasResult);
class SyscallTimingCall {
    SyscallTimingTicket ticket_{};int64_t result_=0;bool hasResult_=false;
public:
    SyscallTimingCall(uint64_t pid,uint64_t incarnation,uint64_t number,uint64_t arg0,SyscallTimingPhase phase=SyscallTimingPhase::Dispatch):ticket_(SyscallTimingEnter(pid,incarnation,number,arg0,phase)){}
    SyscallTimingCall(const SyscallTimingCall&)=delete;
    SyscallTimingCall& operator=(const SyscallTimingCall&)=delete;
    int64_t Result(int64_t result){result_=result;hasResult_=true;return result;}
    ~SyscallTimingCall(){if(ticket_.epoch&1)SyscallTimingLeave(ticket_,result_,hasResult_);}
};
}
