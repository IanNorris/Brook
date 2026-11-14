#include "intel_bdw_wait_queue.h"
#include "intel_bdw_poll_pacer.h"
#include <cassert>
#include <cstdio>
using namespace brook;

int main(){
    // Wait slots: arm against the observed generation, one-shot wake, no wake after disarm.
    {
        BdwWaitSlots<int,2> q;int a=1,b=2,c=3;unsigned sa=9,sb=9,sc=9;
        const uint64_t g=q.Generation();
        assert(q.Arm(&a,g,sa)==BdwWaitArm::Armed && sa==0);
        assert(q.Arm(&b,g,sb)==BdwWaitArm::Armed && sb==1);
        assert(q.Arm(&c,g,sc)==BdwWaitArm::Full && sc==2);
        assert(q.Arm(nullptr,g,sc)==BdwWaitArm::Stale);
        assert(q.Disarm(&b,sb) && !q.Disarm(&b,sb) && q.Armed()==1);
        int woken=0;int* last=nullptr;
        assert(q.Signal([&](int* w){++woken;last=w;})==1 && woken==1 && last==&a);
        assert(q.Generation()==g+1 && q.Armed()==0);
        // Consumed arming: disarm reports false, a second signal wakes nobody.
        assert(!q.Disarm(&a,sa) && q.Signal([&](int*){++woken;})==0 && woken==1);
        // A signal between observing and arming is reported as stale.
        assert(q.Arm(&a,g,sa)==BdwWaitArm::Stale && sa==2);
        const uint64_t now=q.Generation();
        assert(q.Arm(&a,now,sa)==BdwWaitArm::Armed);
        // Wrong slot or wrong waiter never disarms someone else.
        assert(!q.Disarm(&b,sa) && !q.Disarm(&a,7) && q.Disarm(&a,sa));
    }
    // Host event stubs never arm, so ioctl waits keep their sleep counts.
    {
        BdwGpuWaiter w{};const uint64_t g=BdwGpuEventGeneration(BdwGpuEvent::Retire);
        assert(BdwGpuEventArm(BdwGpuEvent::Retire,g,w)==BdwWaitArm::Full);
        BdwGpuFenceSignalled();
        assert(BdwGpuEventArm(BdwGpuEvent::Retire,g,w)==BdwWaitArm::Stale);
        assert(BdwGpuEventGeneration(BdwGpuEvent::Work)!=0);
    }
    // Poll pacer (1000 ticks per ms).
    const BdwPollPacerConfig config{1000,100,50,250};
    {   // Known completion at 3000: sleep while a whole tick plus margin remains.
        BdwPollPacer p;p.Begin(config,10000,3000,2000);
        assert(p.Next(10000)==0 && p.Next(11800)==0 && p.Next(11899)==0);
        assert(p.Next(11901)==50); // inside the window: minimum slice
        p.Spun(60,40);assert(p.Next(11961)==120); // three times the audit cost
        p.Spun(130,200);assert(p.Next(12100)==250); // capped by maxSlice
        // Past the prediction: keep spinning until the budget is spent.
        assert(p.Next(20000)==250);
        for(unsigned i=0;i<100 && p.Next(20000);++i)p.Spun(p.Next(20000),10);
        assert(p.SpunTicks()==2000 && p.Next(20000)==0);
    }
    {   // Unknown completion: spin from the start, bounded by the small budget.
        BdwPollPacer p;p.Begin(config,0,0,120);
        assert(p.Next(0)==50);p.Spun(50,0);assert(p.Next(50)==50);p.Spun(50,0);
        assert(p.Next(100)==20);p.Spun(500,0); // an overrunning slice is clamped
        assert(p.SpunTicks()==120 && p.Next(1000)==0);
    }
    {   // Disabled (zero budget) is exactly the old tick-sleep behaviour.
        BdwPollPacer p;p.Begin(config,0,0,0);assert(p.Next(0)==0 && p.Next(1u<<30)==0);
        BdwPollPacer q;q.Begin(config,0,500,0);assert(q.Next(400)==0);
    }
    {   // Huge audit cost cannot overflow the slice computation.
        BdwPollPacer p;p.Begin({1000,100,50,0},0,0,~uint64_t{0});p.Spun(1,~uint64_t{0});
        assert(p.Next(0)==~uint64_t{0}-1);
    }
    // Completion predictor: falls immediately, rises by a quarter, LRU-replaces.
    {
        BdwCompletionPredictor<2> c;
        assert(c.Predict(1)==0 && c.Predict(0)==0);
        c.Record(0,100);assert(c.Predict(0)==0);
        c.Record(1,4000);assert(c.Predict(1)==4000);
        c.Record(1,3000);assert(c.Predict(1)==3000);
        c.Record(1,7000);assert(c.Predict(1)==4000);
        c.Record(1,0);assert(c.Predict(1)==4000);
        c.Record(2,900);assert(c.Predict(2)==900);
        assert(c.Predict(1)==4000);   // 1 is now most recent
        c.Record(3,500);              // evicts 2
        assert(c.Predict(2)==0 && c.Predict(1)==4000 && c.Predict(3)==500);
    }
    std::puts("test_intel_bdw_wakeups: ok");
    return 0;
}
