#include "../../kernel/src/real_interval_timer.h"
#include <cassert>
#include <cstdio>
using brook::RealIntervalTimer;
int main() {
    RealIntervalTimer timer;
    auto old=timer.Set(100,20,10);assert(!old.remaining && !old.interval);
    assert(timer.Read(110).remaining==10 && timer.Read(110).interval==10);
    assert(timer.Read(120).remaining==1000 && timer.Read(130).remaining==1000);
    assert(!timer.Expire(119) && timer.Expire(120) && timer.Pending());
    assert(!timer.Read(121).remaining && timer.Read(121).interval==10);
    assert(!timer.Expire(150)); // coalesces until the pending signal is consumed
    assert(timer.SharedAlarmDequeued(155));assert(timer.Read(155).remaining==5);
    assert(timer.Expire(160));
    old=timer.Set(161,0,0);assert(!old.remaining && old.interval==10 && timer.Pending());
    assert(timer.SharedAlarmDequeued(170) && !timer.Pending() && !timer.Read(170).remaining);
    timer.Set(200,10,7);assert(timer.Expire(210));
    timer.Set(211,30,0);assert(timer.Pending());assert(timer.SharedAlarmDequeued(212));
    assert(timer.Read(212).remaining==29 && !timer.Read(212).interval);
    assert(timer.Expire(241) && timer.SharedAlarmDequeued(242) && !timer.Read(242).remaining);
    timer.Set(300,10,10);assert(timer.Expire(310));timer.Discard();
    assert(!timer.Pending() && !timer.Read(350).remaining && timer.Read(350).interval==10);
    assert(!timer.SharedAlarmDequeued(355) && timer.Read(355).remaining==5);
    timer.Set(brook::TimeoutMaxNs-5,10,0);
    assert(timer.Read(brook::TimeoutMaxNs-5).remaining==5);
    assert(timer.Expire(brook::TimeoutMaxNs));
    std::puts("REAL_INTERVAL_TIMER_PASS coalescing/reprogramming/phase/saturation");
}
