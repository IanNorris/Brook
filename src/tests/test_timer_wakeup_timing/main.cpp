#include <cstdio>
#include "../../kernel/src/timer_wakeup_timing.h"
namespace {int failed=0;}
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"timer wake line %d: %s\n",__LINE__,#x);++failed;}}while(0)
int main(){
    using namespace brook;TimerWakeTimingModel m;
    CHECK(!m.Begin(0,1,0,0).epoch);m.Enable(true);auto epoch=m.Epoch();
    auto t=m.Begin(epoch,1000000,0x1234,3);
    CHECK(t.epoch==epoch && t.sequence==1 && !t.previousArrivalNs && !t.gapNs);
    CHECK(m.Complete(t,1000010,1000020,1000030,1000040,4));
    auto s=m.Snapshot();CHECK(s.arrivals==1 && s.completed==1 && !s.retained && s.maxScanNs==10);
    t=m.Begin(epoch,71000000,0x4567,0);CHECK(t.gapNs==70000000 && t.previousArrivalNs==1000000);
    CHECK(m.Complete(t,71000001,71000002,71000003,71000004,1));
    s=m.Snapshot();CHECK(s.maxGapNs==70000000 && s.events[0].token.rip==0x4567);
    t=m.Begin(epoch,72000000,1,3);
    CHECK(m.Complete(t,132000000,137000000,144000000,153000000,8));
    s=m.Snapshot();CHECK(s.maxPrefixNs==60000000 && s.maxRuntimeNs==5000000 && s.maxPreScanNs==7000000 && s.maxScanNs==9000000);
    CHECK(s.events[1].prefixNs==60000000 && s.events[1].candidates==8);
    t=m.Begin(epoch,154000000,1,0);
    CHECK(m.Complete(t,154000001,354000001,354000002,354000003,0));
    s=m.Snapshot();CHECK(s.events[0].runtimeNs==200000000 && s.events[0].token.cs==0);
    CHECK(!m.Begin(epoch,1,0,0).epoch);CHECK(!m.Begin(epoch,0,0,0).epoch);
    CHECK(m.Snapshot().invalidClock==2);
    t=m.Begin(epoch,500000000,1,0);CHECK(!t.gapNs);
    CHECK(!m.Complete(t,499999999,500000000,500000001,500000002,0));
    CHECK(!m.Complete(t,500000001,500000000,500000002,500000003,0));
    CHECK(!m.Complete(t,500000001,500000002,500000001,500000003,0));
    CHECK(!m.Complete(t,500000001,500000002,500000003,500000002,0));
    CHECK(m.Snapshot().invalidClock==6);
    m.Reset();CHECK(!m.Complete(t,500000000,500000000,500000000,500000000,0));
    CHECK(!m.Begin(epoch,600000000,0,0).epoch);epoch=m.Epoch();
    t=m.Begin(epoch,1,0,0);m.Enable(false);CHECK(!m.Complete(t,1,2,3,4,0));
    m.Reset();m.Enable(true);epoch=m.Epoch();uint64_t now=1;
    for(unsigned i=0;i<40;++i){
        t=m.Begin(epoch,now,0,0);const uint64_t end=now+20000000+i*1000000;
        CHECK(m.Complete(t,now,end,end,end,0));now=end+1;
    }
    s=m.Snapshot();CHECK(s.arrivals==40 && s.completed==40 && s.slow==40 && s.retained==16 && s.excluded==24);
    CHECK(s.events[0].runtimeNs==59000000 && s.events[15].runtimeNs==44000000);
    CHECK(!s.invalidClock && !s.saturated);
    // Separate stack tokens survive nested arrivals and completion order.
    m.Reset();epoch=m.Epoch();auto a=m.Begin(epoch,1,0,0),b=m.Begin(epoch,2,0,0);
    CHECK(m.Complete(b,3,4,5,6,0));CHECK(m.Complete(a,7,8,9,10,0));
    CHECK(m.Snapshot().completed==2 && m.Snapshot().arrivals==2);
    std::printf("TIMER_WAKE_TIMING %s\n",failed?"FAIL":"PASS");return failed?1:0;
}
