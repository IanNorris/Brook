#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
#include "../../kernel/src/sched_lock_trace.h"
#include "../../kernel/src/irq_accounting.h"
using namespace brook;
static int failures=0;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"lock trace line %d: %s\n",__LINE__,#x);++failures;}}while(0)
static RuntimeSample At(uint64_t wall,uint64_t steal=0){return {wall,steal,true};}
int main(){
    SchedulerLockTrace trace;CHECK(!trace.Enabled() && !trace.HasActive());
    CHECK(!trace.Reserve(0,0,At(1),true,123));CHECK(trace.Control(true));
    const auto state=trace.State();auto first=trace.Reserve(0,UINT32_MAX,At(10,1),true,123);
    CHECK(first && trace.HasActive());CHECK(!trace.Reserve(0,1,At(11),false,0)); // same-lock reentry is diagnostic loss
    trace.Acquired(first,At(20,3));auto c=trace.Read(0);
    CHECK(c.currentValid && c.current.phase==2 && c.current.caller==123 && c.lost==1);
    CHECK(trace.Control(false));CHECK(!trace.Enabled());
    auto release=trace.Releasing(0,UINT32_MAX,At(50,20));CHECK(release);
    trace.Released(release,At(52,20));c=trace.Read(0);
    CHECK(!trace.HasActive() && c.completed==1 && c.current.phase==4 && c.current.flags==31);
    CHECK(c.current.epoch==(state&~uint64_t(1)) && c.current.releaseAfterNs==52);
    CHECK(c.waitValid && c.holdValid && c.maxWait.serial==first.serial && c.maxHold.serial==first.serial);
    CHECK(trace.Control(true));auto second=trace.Reserve(0,0,At(100),false,456); // ticket wraps; local serial must not
    CHECK(second && second.serial==first.serial+1 && second.epoch!=first.epoch);
    trace.Acquired(first,At(110));CHECK(trace.Read(0).current.phase==1); // stale token cannot close reused ticket
    CHECK(trace.Control(true,true));const auto newState=trace.State(); // reset while waiting
    trace.Acquired(second,At(120));release=trace.Releasing(0,0,At(150));trace.Released(release,At(151));
    c=trace.Read(0);CHECK(c.current.epoch==second.epoch && c.current.epoch!=(newState&~uint64_t(1)));
    auto third=trace.Reserve(0,1,{},false,0);CHECK(third);trace.Acquired(third,{});
    CHECK(trace.Control(false,true));release=trace.Releasing(0,1,{});trace.Released(release,{});
    c=trace.Read(0);CHECK(c.current.phase==4 && !c.current.flags && c.completed==3 && c.clockErrors==1);
    CHECK(!trace.Reserve(64,1,At(1),false,0));CHECK(!trace.Releasing(64,0,At(1)));
    CHECK(!trace.Read(64).currentValid);CHECK(trace.Control(true));
    CHECK(!trace.Reserve(0,2,At(1),false,0,newState)); // stale enable/sample window
    uint64_t stateOutput=123;
    CHECK(!SchedulerLockTrace::NextState(~uint64_t(0)-2,true,false,&stateOutput) && stateOutput==123);
    CHECK(SchedulerLockTrace::NextState(~uint64_t(0)-2,false,false,&stateOutput) && stateOutput==~uint64_t(0)-3);
    CHECK(!SchedulerLockTrace::NextState(0,true,false,nullptr));
    CHECK(trace.Control(false));const auto disabledState=trace.State();
    CHECK(trace.Control(-1,true) && !trace.Enabled() && trace.State()==disabledState+2);
    CHECK(trace.Control(true));const auto enabledState=trace.State();
    CHECK(trace.Control(-1,true) && trace.Enabled() && trace.State()==enabledState+2);
    const auto unchanged=trace.State();CHECK(!trace.Control(-2,true) && !trace.Control(2,true) && trace.State()==unchanged);
    for(unsigned i=0;i<3;++i){
        const uint64_t start=1000*(i+1),wait=i==1?30:i==0?10:1,hold=i==0?50:i==1?5:4;
        auto token=trace.Reserve(63,i,At(start),false,0);trace.Acquired(token,At(start+wait));
        auto done=trace.Releasing(63,i,At(start+wait+hold));trace.Released(done,At(start+wait+hold+1));
    }
    auto retained=trace.Read(63);CHECK(retained.current.serial==3 && retained.maxWait.serial==2 && retained.maxHold.serial==1);
    CHECK(!retained.clockErrors && retained.completed==3 && !retained.lost);
    auto rewind=trace.Reserve(62,0,At(100),true,0);trace.Acquired(rewind,At(90));
    auto rewindEnd=trace.Releasing(62,0,At(80));trace.Released(rewindEnd,At(70));
    CHECK(trace.Read(62).clockErrors==1 && trace.Read(62).current.releaseAfterNs==70);

    // Real try-claim helper: only successful reservations publish, including
    // stale success across a complete uint32 ticket-counter cycle.
    volatile uint32_t next=0,serving=0;unsigned reserved=0,waited=0;
    CHECK(TicketTryClaim(&next,&serving,[&](uint32_t){++waited;},TicketNoHook{},[&](uint32_t ticket){CHECK(ticket==0);++reserved;}));
    CHECK(!TicketTryClaim(&next,&serving,[&](uint32_t){++waited;},TicketNoHook{},[&](uint32_t){++reserved;}));
    CHECK(reserved==1 && waited==0);
    next=serving=0;
    CHECK(TicketTryClaim(&next,&serving,[&](uint32_t ticket){++waited;CHECK(ticket==0);serving=ticket;},
        [&]{next=0;serving=1;},[&](uint32_t ticket){CHECK(ticket==0);++reserved;}));
    CHECK(reserved==2 && waited==1);

    // Bounded publication failure leaves the caller's output unchanged.
    LockTracePublication publication;LockTraceRecord sentinel{};sentinel.serial=999;
    __atomic_store_n(&publication.sequence,1,__ATOMIC_RELAXED);
    CHECK(!publication.Read(&sentinel) && sentinel.serial==999 && !publication.Store({}));
    __atomic_store_n(&publication.sequence,~uint64_t(0)-1,__ATOMIC_RELAXED);
    CHECK(!publication.Store({}) && !publication.Read(nullptr));

    // Actual atomic field publication with concurrent readers: an accepted
    // snapshot must have one complete version, never a mixture of versions.
    LockTracePublication shared;std::atomic<bool> done{false},bad{false};
    std::thread writer([&]{for(uint64_t i=1;i<=100000;++i){LockTraceRecord r{};r.serial=i;r.ticket=i^0x55aa;
        r.epoch=i+7;r.phase=i+11;r.reservedNs=i+13;r.releaseAfterNs=i+17;r.flags=i+19;
        if(!shared.Store(r))bad=true;}done=true;});
    std::vector<std::thread> readers;
    for(unsigned n=0;n<4;++n)readers.emplace_back([&]{do{LockTraceRecord r{};if(shared.Read(&r)&&r.serial)
        if(r.ticket!=(r.serial^0x55aa)||r.epoch!=r.serial+7||r.phase!=r.serial+11||
           r.reservedNs!=r.serial+13||r.releaseAfterNs!=r.serial+17||r.flags!=r.serial+19)bad=true;
    }while(!done.load());});
    writer.join();for(auto& reader:readers)reader.join();CHECK(!bad.load());

    // Independent CPU publishers complete their original epoch while a control
    // thread disables/resets the window. Readers never clear live records.
    SchedulerLockTrace concurrent;CHECK(concurrent.Control(true));
    std::atomic<unsigned> ready{0},finished{0};std::atomic<bool> go{false};
    std::vector<std::thread> cpus;
    for(unsigned cpu=0;cpu<4;++cpu)cpus.emplace_back([&,cpu]{
        auto episode=[&](unsigned i){
            auto token=concurrent.Reserve(cpu,i,At(i*10+1),false,i);if(!token)return;
            concurrent.Acquired(token,At(i*10+2));
            auto released=concurrent.Releasing(cpu,i,At(i*10+3));
            if(!released){bad=true;return;}concurrent.Released(released,At(i*10+4));
        };
        episode(1);++ready;while(!go.load())std::this_thread::yield();
        for(unsigned i=2;i<2000;++i)episode(i);++finished;
    });
    while(ready.load()!=4)std::this_thread::yield();go=true;
    unsigned controls=0;
    while(finished.load()!=4){
        concurrent.Control((controls++%3)!=0,true);
        for(unsigned cpu=0;cpu<4;++cpu){auto snap=concurrent.Read(cpu);
            for(const auto& r:{snap.current,snap.maxWait,snap.maxHold}){
                // A rejected publication's output remains the zero initializer.
                if(!r.phase)continue;
                if(r.reservedNs!=r.caller*10+1 || r.ticket!=r.caller || !r.serial || !r.epoch)bad=true;
                if(r.phase>=2 && r.acquiredNs!=r.caller*10+2)bad=true;
                if(r.phase>=3 && r.releaseBeforeNs!=r.caller*10+3)bad=true;
                if(r.phase>=4 && r.releaseAfterNs!=r.caller*10+4)bad=true;
            }
        }
    }
    for(auto& cpu:cpus)cpu.join();CHECK(!concurrent.HasActive() && !bad.load());
    for(unsigned cpu=0;cpu<4;++cpu){auto snap=concurrent.Read(cpu);CHECK(snap.completed>=1 && !snap.lost && !snap.clockErrors);}
    // A predecessor finishing its post-unlock publication cannot clear a
    // successor CPU's active reservation/acquisition bit.
    SchedulerLockTrace handoff;CHECK(handoff.Control(true));
    auto owner=handoff.Reserve(0,10,At(1),false,0);handoff.Acquired(owner,At(2));
    auto oldRelease=handoff.Releasing(0,10,At(3));
    auto successor=handoff.Reserve(1,11,At(4),false,0);handoff.Acquired(successor,At(5));
    CHECK(handoff.ActiveMask()==3);handoff.Released(oldRelease,At(6));CHECK(handoff.ActiveMask()==2);
    auto newRelease=handoff.Releasing(1,11,At(7));handoff.Released(newRelease,At(8));CHECK(!handoff.HasActive());
    std::printf("SCHED_LOCK_TRACE %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
