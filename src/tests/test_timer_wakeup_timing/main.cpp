#include <cstdio>
#include "../../kernel/src/timer_wakeup_timing.h"
#include "../../kernel/src/timer_irq_arrivals.h"
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
    // Steal pairing is explicit: first/missing/backwards pairs cannot be
    // interpreted as zero host delay. Never silently clamp a delta to the gap.
    m.Reset();epoch=m.Epoch();
    t=m.Begin(epoch,1000000000,1,3,true,100);
    CHECK(t.hasSteal && !t.hasStealGap);
    CHECK(!m.Complete(t,1000000001,1000000002,1000000003,1000000004,3,0,0));
    CHECK(m.Complete(t,1000000001,1000000002,1000000003,1000000004,3,1090000004,2));
    s=m.Snapshot();CHECK(s.maxTailNs==90000000 && s.migrated==1 && s.events[0].completionCpu==2);
    t=m.Begin(epoch,1100000000,1,3,true,100000100);
    CHECK(t.hasStealGap && t.stealDeltaNs==100000000 && t.gapNs==100000000);
    CHECK(m.Complete(t,1100000001,1100000002,1100000003,1100000004,0));
    t=m.Begin(epoch,1200000000,1,3,true,100000100);
    CHECK(t.hasStealGap && !t.stealDeltaNs);
    t=m.Begin(epoch,1300000000,1,3,true,99);
    CHECK(t.hasSteal && !t.hasStealGap && m.Snapshot().backwardsSteal==1);
    t=m.Begin(epoch,1400000000,1,3,false,0);
    CHECK(!t.hasSteal && !t.hasStealGap && m.Snapshot().missingStealEntries==1);
    t=m.Begin(epoch,1500000000,1,3,true,200);CHECK(!t.hasStealGap);
    t=m.Begin(epoch,1600000000,1,3,true,500000000);
    CHECK(t.hasStealGap && t.stealDeltaNs==499999800 && m.Snapshot().stealExceedsGap==1);
    CHECK(m.Complete(t,1600000001,1600000002,1600000003,1600000004,0));
    m.Reset();epoch=m.Epoch();t=m.Begin(epoch,1,0,0,true,500000000);
    CHECK(!t.previousArrivalNs && !t.hasStealGap && !m.Snapshot().missingStealEntries);
    // Per-CPU arrival histories stay independent, and disabling a window
    // preserves evidence while rejecting in-flight samples from its old epoch.
    TimerIrqArrivalModel cpu[2];cpu[0].Reset(3);cpu[1].Reset(3);
    cpu[0].Observe(1,100,0,0);cpu[0].Observe(2,100,0,0);
    CHECK(!cpu[0].Snapshot().arrivals);
    cpu[0].Observe(3,1000000,1,3);cpu[1].Observe(3,1000000,2,3);
    cpu[0].Observe(3,101000000,3,8);
    for(uint64_t n=2000000;n<=101000000;n+=1000000)cpu[1].Observe(3,n,4,3);
    auto p=cpu[0].Snapshot(),q=cpu[1].Snapshot();
    CHECK(p.arrivals==2 && p.gaps==1 && p.slow==1 && p.maxGapNs==100000000);
    CHECK(p.maxGapStartNs==1000000 && p.maxGapEndNs==101000000 && p.maxGapRip==3 && p.maxGapCs==8);
    CHECK(q.arrivals==101 && q.gaps==100 && !q.slow && q.maxGapNs==1000000);
    cpu[0].SetEpoch(4);cpu[0].Observe(3,201000000,5,3);cpu[0].Observe(4,201000000,5,3);
    CHECK(cpu[0].Snapshot().arrivals==2 && cpu[0].Snapshot().lastNs==101000000);
    cpu[0].Reset(7);cpu[0].Observe(3,1,0,0);CHECK(!cpu[0].Snapshot().arrivals);
    cpu[0].Observe(7,100,0,0);cpu[0].Observe(7,0,0,0);cpu[0].Observe(7,200,0,0);
    CHECK(cpu[0].Snapshot().invalidClock==1 && cpu[0].Snapshot().gaps==0);
    cpu[0].Observe(7,199,0,0);cpu[0].Observe(7,300,0,0);cpu[0].Observe(7,300,0,0);
    p=cpu[0].Snapshot();CHECK(p.invalidClock==2 && p.arrivals==4 && p.gaps==1 && p.maxGapNs==0);
    CHECK(p.firstNs==100 && p.lastNs==300 && !p.saturated);
    std::printf("TIMER_WAKE_TIMING %s\n",failed?"FAIL":"PASS");return failed?1:0;
}
