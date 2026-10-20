#include <cstdio>
#include "../../kernel/src/dsp_producer_timing.h"
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
