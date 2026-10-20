#include <cstdio>
#include "../../kernel/src/audio_service_timing.h"
namespace {int failures=0;}
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"audio timing line %d: %s\n",__LINE__,#x);++failures;}}while(0)
int main(){
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
