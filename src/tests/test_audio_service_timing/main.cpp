#include <cstdio>
#include "../../kernel/src/dsp_producer_timing.h"
#include "../../kernel/src/syscall_timing.h"
#include "../../kernel/src/sched_decision_trace.h"
#include "../../kernel/src/audio_service_timing.h"
namespace {int failures=0;}
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"audio timing line %d: %s\n",__LINE__,#x);++failures;}}while(0)
namespace brook {
static DspProducerTimingModel producer;
static uint64_t producerNow=0,producerClockReads=0;
extern "C" uint64_t AudioClockNowNs(){++producerClockReads;return producerNow;}
uint64_t DspProducerTimingToken(){return producer.Token();}
void DspProducerTimingRecord(uint64_t token,const DspProducerEvent& event){producer.Observe(token,event);}
}
namespace brook {
static SyscallTimingModel syscallProbe;
static uint64_t syscallNow=0,syscallClockReads=0;
SyscallTimingTicket SyscallTimingEnter(uint64_t pid,uint64_t incarnation,uint64_t number,uint64_t arg0,SyscallTimingPhase phase){
    auto s=syscallProbe.Snapshot();if(!(s.epoch&1) || s.pid!=pid || s.incarnation!=incarnation)return {};
    ++syscallClockReads;return syscallProbe.Begin(s.epoch,pid,incarnation,number,arg0,syscallNow,phase);
}
void SyscallTimingLeave(SyscallTimingTicket ticket,int64_t result,bool hasResult){++syscallClockReads;syscallProbe.End(ticket,syscallNow,result,hasResult);}
}
static void CheckDecisions(){
    using namespace brook;SchedDecisionModel m;SchedDecisionEvent e{};
    CHECK(!m.Observe(e));m.Watch(22,100);auto epoch=m.Snapshot().epoch;
    e.epoch=epoch;e.pid=22;e.incarnation=100;e.nowNs=300;e.generation=1;e.usedNs=20000000;e.quantumNs=20000000;
    e.priority=2;e.reason=MlfqDecisionReason::Service;e.serviceSeen=true;
    e.hasDispatch=true;e.hasDrain=true;e.dispatch={100,10,true};e.drained={200,20,true};
    CHECK(m.Observe(e));auto snap=m.Snapshot();CHECK(snap.observations==1 && snap.serviceExpiry==1 && !snap.invalid && !snap.unpaired);
    e.incarnation=101;CHECK(!m.Observe(e));e.incarnation=100;
    e.reason=MlfqDecisionReason::HigherPriority;e.higherPriority=1;e.usedNs=1;CHECK(m.Observe(e));
    snap=m.Snapshot();CHECK(snap.higherPriority==1 && snap.events[0].usedNs==20000000);
    e.higherPriority=2;CHECK(!m.Observe(e));e.higherPriority=1;
    e.usedNs=e.quantumNs;CHECK(!m.Observe(e));e.usedNs=1;
    e.reason=MlfqDecisionReason::LegacyWall;CHECK(!m.Observe(e));e.serviceSeen=false;CHECK(m.Observe(e));
    e.drained.wallNs=301;CHECK(!m.Observe(e));e.drained.wallNs=99;CHECK(!m.Observe(e));e.drained={200,9,true};CHECK(!m.Observe(e));
    e.drained={200,20,true};e.dispatch.valid=false;CHECK(!m.Observe(e));e.dispatch.valid=true;
    e.hasDispatch=false;CHECK(!m.Observe(e));e.hasDrain=false;e.dispatch={};e.drained={};CHECK(m.Observe(e));CHECK(m.Snapshot().unpaired==1);
    m.Reset();CHECK(!m.Observe(e));e.epoch=m.Snapshot().epoch;
    for(unsigned i=0;i<40;++i){e.usedNs=i;CHECK(m.Observe(e));}
    snap=m.Snapshot();CHECK(snap.observations==40 && snap.wallExpiry==40 && snap.retained==32 && snap.excluded==8 && snap.maxUsedNs==39);
    CHECK(snap.events[0].usedNs==39 && snap.events[31].usedNs==8);
    e.generation=~uint64_t(0);CHECK(!m.Observe(e) && m.Snapshot().saturated);
    m.Off();e.epoch=m.Snapshot().epoch;CHECK(!m.Observe(e));
    m.Watch(23,101);e.epoch=m.Snapshot().epoch;e.generation=1;CHECK(!m.Observe(e));
}
static void CheckSyscalls(){
    using namespace brook;
    {SyscallTimingCall call(22,100,1,7);CHECK(call.Result(-11)==-11);}
    CHECK(!syscallClockReads && !syscallProbe.Snapshot().entries);
    syscallProbe.Watch(22,100);syscallNow=100;
    {SyscallTimingCall wrong(22,101,1,7);wrong.Result(1);}
    CHECK(!syscallClockReads);
    {SyscallTimingCall call(22,100,1,7);CHECK(call.Result(1024)==1024);syscallNow=200;}
    CHECK(syscallProbe.Snapshot().completed==1 && syscallClockReads==2);
    {SyscallTimingCall call(22,100,1,7);CHECK(call.Result(-11)==-11);syscallNow=300;}
    CHECK(syscallProbe.Snapshot().kind[0].negativeResults==1);
    {SyscallTimingCall noResult(22,100,1,7);syscallNow=400;}
    CHECK(syscallProbe.Snapshot().invalid==1);
    {SyscallTimingCall crossing(22,100,1,7);crossing.Result(1);syscallProbe.Reset();syscallNow=500;}
    CHECK(!syscallProbe.Snapshot().completed && !syscallProbe.Snapshot().invalid);
    SyscallTimingModel m;CHECK(!m.Begin(0,22,100,1,7,1).epoch);m.Watch(22,100);auto epoch=m.Token();
    CHECK(!m.Begin(epoch,22,101,1,7,1).epoch);
    auto first=m.Begin(epoch,22,100,1,7,100000000);CHECK(first.epoch==epoch);
    auto snap=m.Snapshot();CHECK(snap.active && snap.activeNumber==1 && snap.activeArg0==7 && snap.entries==1 && !snap.completed);
    CHECK(!m.End({epoch,first.sequence+1},150000000,1) && m.Snapshot().active);
    CHECK(m.End(first,150000000,4096));CHECK(!m.End(first,160000000,4096));
    auto second=m.Begin(epoch,22,100,202,123,200000000);CHECK(m.End(second,500000000,-11));
    snap=m.Snapshot();CHECK(snap.completed==2 && snap.kinds==2 && snap.maxCallNs==300000000 && snap.maxGapNs==50000000);
    CHECK(snap.events[0].number==202 && snap.events[0].result==-11 && snap.events[0].arg0==123 && snap.events[0].hasGap);
    CHECK(snap.kind[0].calls==1 && snap.kind[0].sumNs==50000000 && snap.kind[1].negativeResults==1);
    // Missing completion is explicit; a later entry must not fabricate a gap.
    auto abandoned=m.Begin(epoch,22,100,7,0,600000000);auto replacement=m.Begin(epoch,22,100,1,7,700000000);
    CHECK(!m.End(abandoned,800000000,1) && m.Snapshot().active);CHECK(m.End(replacement,800000000,1));
    CHECK(m.Snapshot().incomplete==1 && !m.Snapshot().active);
    auto bad=m.Begin(epoch,22,100,1,7,900000000);CHECK(!m.End(bad,899999999,1) && m.Snapshot().invalid==1);
    auto beforeReset=m.Begin(epoch,22,100,1,7,1000000000);m.Reset();CHECK(!m.End(beforeReset,1100000000,1));
    CHECK(!m.Snapshot().active && !m.Snapshot().completed);epoch=m.Token();
    auto active=m.Begin(epoch,22,100,1,7,100);CHECK(!m.Begin(epoch,22,100,7,0,99).epoch);
    CHECK(m.Snapshot().invalid==1 && m.Snapshot().incomplete==1 && !m.End(active,200,1));
    m.Watch(22,101);epoch=m.Token();CHECK(!m.Begin(epoch,22,100,1,7,1).epoch);
    auto identity=m.Begin(epoch,22,101,1,7,1);m.Off();CHECK(!m.End(identity,2,1));
    m.Watch(22,101);epoch=m.Token();uint64_t now=1;
    for(unsigned i=0;i<40;++i){auto call=m.Begin(epoch,22,101,i,i,now);now+=20000000+i;CHECK(m.End(call,now,0));++now;}
    snap=m.Snapshot();CHECK(snap.completed==40 && snap.kinds==16 && snap.overflowKinds==24 && snap.retained==16 && snap.excluded==24);
    CHECK(snap.events[0].endNs-snap.events[0].beginNs==20000039);
    m.Reset();epoch=m.Token();auto large=m.Begin(epoch,22,101,1,7,1);CHECK(m.End(large,~uint64_t(0),1));
    CHECK(!m.Begin(epoch,22,101,1,7,1).epoch);large=m.Begin(epoch,22,101,1,7,1);CHECK(m.End(large,~uint64_t(0),1));
    CHECK(m.Snapshot().saturated && m.Snapshot().kind[0].sumNs==~uint64_t(0));
    m.Watch(22,100);epoch=m.Token();CHECK(m.Matches(22,100) && !m.Matches(22,101) && !m.Matches(23,100));
    auto dispatch=m.Begin(epoch,22,100,7,0,100);CHECK(m.End(dispatch,200,1));
    auto returning=m.Begin(epoch,22,100,7,0,201,SyscallTimingPhase::ReturnCheck);CHECK(m.End(returning,30000000,1));
    auto next=m.Begin(epoch,22,100,1,7,30000100);CHECK(m.End(next,30000200,4096));
    snap=m.Snapshot();CHECK(snap.kinds==3 && snap.completed==3 && snap.kind[1].number==7 && snap.kind[1].phase==SyscallTimingPhase::ReturnCheck);
    CHECK(snap.events[0].phase==SyscallTimingPhase::ReturnCheck && snap.events[0].number==7 && snap.maxGapNs==100);
    // Missing entry between completed phases clears the next gap, even when
    // its corresponding completion was never observed.
    m.BreakContinuity();auto afterDrop=m.Begin(epoch,22,100,7,0,90000000);CHECK(m.End(afterDrop,90000001,0));
    CHECK(m.Snapshot().continuityBreaks==1 && m.Snapshot().maxGapNs==100);
    // A drop during an observed call also invalidates its pre-entry gap.
    auto activeDrop=m.Begin(epoch,22,100,7,0,190000000);m.BreakContinuity();CHECK(m.End(activeDrop,220000000,0));
    CHECK(!m.Snapshot().events[0].hasGap && m.Snapshot().continuityBreaks==2);
    m.Watch(22,100);epoch=m.Token();
    auto invalidNumber=m.Begin(epoch,22,100,~uint64_t(0),0,1);CHECK(m.End(invalidNumber,2,-38));
    auto returnInvalid=m.Begin(epoch,22,100,~uint64_t(0),0,3,SyscallTimingPhase::ReturnCheck);CHECK(m.End(returnInvalid,4,-38));
    CHECK(m.Snapshot().kinds==2 && m.Snapshot().kind[0].phase==SyscallTimingPhase::Dispatch && m.Snapshot().kind[1].phase==SyscallTimingPhase::ReturnCheck);
    m.BreakContinuity(true);CHECK(m.Snapshot().saturated);
    m.Off();CHECK(!m.Matches(22,100));
}
static void CheckProducer(){
    using namespace brook;
    {DspProducerCall call(1,10,20,4096);CHECK(call.Result(-11)==-11);CHECK(call.Mark()==0);}
    CHECK(!producerClockReads && !producer.Snapshot().completed);
    producer.Enable(true);producerNow=100000000;
    {
        DspProducerCall call(1,10,20,4096);
        producerNow=110000000;auto before=call.Mark();producerNow=120000000;call.Mutex(before);
        producerNow=130000000;before=call.Mark();producerNow=160000000;call.Sleep(before);
        CHECK(call.Result(1024)==1024);producerNow=170000000;
    }
    auto s=producer.Snapshot();CHECK(s.completed==1 && s.acceptedBytes==1024 && !s.errors && !s.invalid);
    CHECK(s.sumCallNs==70000000 && s.sumMutexNs==10000000 && s.sumBackpressureNs==30000000);
    CHECK(s.retained==1 && !s.events[0].hasGap && s.events[0].attempts==1 && s.events[0].sleeps==1);
    CHECK(s.events[0].endNs-s.events[0].beginNs-s.events[0].mutexNs-s.events[0].backpressureNs==30000000);
    producerNow=300000000;
    {DspProducerCall call(1,10,20,4096);CHECK(call.Result(-32)==-32);producerNow=310000000;}
    s=producer.Snapshot();CHECK(s.maxGapNs==130000000 && s.completed==2 && s.errors==1 && s.acceptedBytes==1024);
    CHECK(s.events[0].hasGap && s.events[0].gapNs==130000000 && s.events[0].result==-32);
    // Reset/control boundaries reject the whole old call, including local spans.
    {DspProducerCall call(1,10,20,4096);producer.Reset();call.Result(4096);producerNow+=100;}
    CHECK(!producer.Snapshot().completed && !producer.Snapshot().invalid);
    // Missing terminal result is explicit invalid data; no fabricated byte count.
    {DspProducerCall call(1,10,20,4096);producerNow+=100;}
    CHECK(producer.Snapshot().invalid==1 && !producer.Snapshot().completed);
    producer.Reset();producerNow=1000;
    {DspProducerCall call(1,10,20,4096);call.Result(4096);producerNow=500;}
    CHECK(producer.Snapshot().invalid==1);
    producerNow=2000;{DspProducerCall call(1,10,20,4096);call.Result(4096);producerNow=3000;}
    CHECK(producer.Snapshot().completed==1 && !producer.Snapshot().maxGapNs);
    producerNow=4000;
    {DspProducerCall call(1,10,20,4096);producerNow=5000;call.Mark();producerNow=4500;call.Mark();producerNow=6000;call.Result(4096);}
    CHECK(producer.Snapshot().invalid==2 && producer.Snapshot().completed==1);
    // Identity reuse and independent streams cannot inherit another writer's gap.
    producerNow=4000;{DspProducerCall call(1,11,20,4096);call.Result(4096);producerNow=5000;}
    producerNow=6000;{DspProducerCall call(1,10,21,4096);call.Result(4096);producerNow=7000;}
    CHECK(producer.Snapshot().writers==3 && !producer.Snapshot().maxGapNs);
    DspProducerTimingModel m;m.Enable(true);auto token=m.Token();
    DspProducerEvent e{};e.pid=1;e.incarnation=1;e.stream=1;e.beginNs=100;e.endNs=200;e.requested=4096;e.result=4096;e.hasResult=true;
    auto bad=e;bad.pid=0;CHECK(!m.Observe(token,bad) && !m.Snapshot().writers);
    bad=e;bad.mutexNs=80;bad.backpressureNs=30;CHECK(!m.Observe(token,bad));
    bad=e;bad.result=4097;CHECK(!m.Observe(token,bad));
    CHECK(m.Observe(token,e));CHECK(!m.Observe(token,e)); // overlapping call cannot form a negative gap
    for(unsigned i=2;i<=8;++i){e.pid=i;CHECK(m.Observe(token,e));}
    e.pid=9;CHECK(!m.Observe(token,e) && m.Snapshot().overflowWriters==1);
    CHECK(m.Snapshot().writers==8 && m.Snapshot().completed==8);
    m.Reset();token=m.Token();e.pid=1;e.beginNs=1;
    for(unsigned i=0;i<40;++i){e.endNs=e.beginNs+20000000+i;CHECK(m.Observe(token,e));e.beginNs=e.endNs+1;}
    CHECK(m.Snapshot().retained==16 && m.Snapshot().excluded==24 && m.Snapshot().slow==40);
    CHECK(m.Snapshot().events[0].endNs-m.Snapshot().events[0].beginNs==20000039);
    m.Reset();token=m.Token();e.beginNs=1;e.endNs=~uint64_t(0);CHECK(m.Observe(token,e));
    m.Enable(false);m.Enable(true);token=m.Token();CHECK(m.Observe(token,e));
    CHECK(m.Snapshot().saturated && m.Snapshot().sumCallNs==~uint64_t(0));
}
int main(){
    CheckProducer();
    CheckDecisions();CheckSyscalls();
    using namespace brook;AudioServiceTimingModel m;
    CHECK(!m.Observe(0,1,2,3,4,true));m.Worker(8,99);m.Enable(true);auto token=m.Token();
    CHECK(m.Observe(token,100,200,10000200,10001200,true));
    auto s=m.Snapshot();CHECK(s.cycles==1 && s.worked==1 && s.sumSleepNs==10000000);
    CHECK(s.maxLateLowerNs==0 && s.maxServiceNs==1000 && !s.retained);
    CHECK(m.Observe(token,10001201,10001202,212001202,212002202,true));
    s=m.Snapshot();CHECK(s.maxLateLowerNs==192000000 && s.events[0].lateLowerNs==192000000);
    CHECK(s.events[0].serviceNs==1000 && s.retained==1);
    CHECK(m.Observe(token,212002203,212002204,222002204,424002204,true));
    s=m.Snapshot();CHECK(s.events[0].serviceNs==202000000 && s.events[0].lateLowerNs==0);
    CHECK(s.events[1].lateLowerNs==192000000);
    // The gap after a completed service may be diagnostic/host/scheduler tail,
    // not the following sleep or service. Preserve that distinct bracket.
    CHECK(m.Observe(token,624002204,624002205,634002205,634002206,false));
    s=m.Snapshot();CHECK(s.maxTailNs==200000000 && s.events[1].tailNs==200000000);
    const auto cycles=s.cycles;
    CHECK(!m.Observe(token,0,1,2,3,true));CHECK(!m.Observe(token,20,10,30,40,true));
    CHECK(!m.Observe(token,40,50,45,60,true));CHECK(!m.Observe(token,50,60,70,65,true));
    CHECK(!m.Observe(token,1,~uint64_t(0),~uint64_t(0),~uint64_t(0),true));
    CHECK(m.Snapshot().invalidClock==5 && m.Snapshot().cycles==cycles);
    m.Reset();CHECK(m.Snapshot().workerPid==8 && m.Snapshot().workerIncarnation==99);
    CHECK(!m.Observe(token,1,2,3,4,true));token=m.Token();
    CHECK(m.Observe(token,1,40000001,50000001,50000002,true));CHECK(m.Snapshot().maxSetupNs==40000000);
    m.Enable(false);CHECK(!m.Observe(token,1,2,3,4,true));m.Reset();m.Enable(true);token=m.Token();
    uint64_t now=1;
    for(unsigned i=0;i<40;++i){
        const uint64_t work=AudioServiceSlowNs+i;
        CHECK(m.Observe(token,now,now,now+AudioServicePeriodNs,now+AudioServicePeriodNs+work,true));
        now+=AudioServicePeriodNs+work+1;
    }
    s=m.Snapshot();CHECK(s.slowCycles==40 && s.retained==AudioServiceEventSlots && s.excludedSlow==24);
    CHECK(s.events[0].serviceNs==AudioServiceSlowNs+39 && s.events[15].serviceNs==AudioServiceSlowNs+24);
    CHECK(!s.saturated && !s.invalidClock);
    // Non-overflowing input intervals can still saturate aggregate sums.
    m.Reset();token=m.Token();
    CHECK(m.Observe(token,1,1,AudioServicePeriodNs+1,~uint64_t(0),true));
    CHECK(!m.Observe(token,1,1,AudioServicePeriodNs+1,~uint64_t(0),true));
    CHECK(m.Snapshot().invalidClock==1); // backwards cycle cannot manufacture sums
    m.Enable(false);m.Enable(true);token=m.Token();
    CHECK(m.Observe(token,1,1,AudioServicePeriodNs+1,~uint64_t(0),true));
    CHECK(m.Snapshot().saturated && m.Snapshot().sumServiceNs==~uint64_t(0));
    std::printf("AUDIO_SERVICE_TIMING %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
