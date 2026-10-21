#include <array>
#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>
#include "../../kernel/src/irq_accounting.h"
#include "../../kernel/src/runtime_accounting.h"
namespace {
int failures=0;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"RUNTIME line %d: %s\n",__LINE__,#x);++failures;}}while(0)
brook::RuntimeSample at(uint64_t wall, uint64_t steal=0) {return {wall,steal,true};}
}
int main() {
    using brook::RuntimeAccounting;
    uint64_t sampledSteal=0;
    unsigned wallReads=0, stealReads=0;
    auto pair=brook::RuntimeReadSample(true,[&](uint64_t* wall) {
        ++wallReads;*wall=wallReads*100;
        if(wallReads==1) sampledSteal=40; // preempted between first steal and wall
        return true;
    },[&](uint64_t* steal){++stealReads;*steal=sampledSteal;return true;});
    CHECK(pair.valid && pair.wallNs==200 && pair.stealNs==40 && pair.attempts==2 && wallReads==2 && stealReads==4);
    wallReads=0;
    pair=brook::RuntimeReadSample(true,[&](uint64_t* wall){++wallReads;*wall=1000;++sampledSteal;return true;},
        [&](uint64_t* steal){*steal=sampledSteal;return true;});
    CHECK(!pair.valid && wallReads==8); // no unbounded retry while holding ready lock
    pair=brook::RuntimeReadSample(false,[](uint64_t* wall){*wall=42;return true;},
        [](uint64_t*){return false;});
    CHECK(pair.valid && pair.wallNs==42 && pair.stealNs==0);
    CHECK(!brook::RuntimeReadSample(false,[](uint64_t*){return false;},
        [](uint64_t*){return false;}).valid);
    uint64_t value=999;
    RuntimeAccounting<16,2> ledger;
    CHECK(!ledger.Register(0,1,1,1));
    CHECK(!ledger.Register(16,1,1,1));
    CHECK(ledger.Register(1,101,1,101));
    CHECK(ledger.Register(2,102,1,101));
    ledger.Dispatch(0,1,101,at(1000));
    CHECK(ledger.Sample(0,at(2300,300))==1000);
    CHECK(ledger.Read(1,101,false,&value) && value==1000);
    ledger.Dispatch(0,0,0,at(3300,300));
    ledger.Sample(0,at(103300,300)); // sleeping task does not advance
    CHECK(ledger.Read(1,101,false,&value) && value==2000);
    CHECK(ledger.Diagnostics().idleNs==100000);
    CHECK(ledger.Diagnostics().observedStealNs==300);
    ledger.Dispatch(1,2,102,at(1000));
    ledger.Dispatch(1,0,0,at(6000));
    CHECK(ledger.Read(2,102,false,&value) && value==5000);
    CHECK(ledger.Read(1,101,true,&value) && value==7000);
    CHECK(ledger.Register(1,101,1,101)); // policy replacement/exec preserves totals
    CHECK(ledger.Read(1,101,true,&value) && value==7000);
    CHECK(ledger.Register(2,202,2,202)); // new PID generation, previous group's total retained
    CHECK(ledger.Read(2,202,false,&value) && value==0);
    CHECK(!ledger.Read(2,102,false,&value));
    CHECK(ledger.Read(1,101,true,&value) && value==7000);
    ledger.Dispatch(1,2,202,at(7000));
    ledger.Sample(1,at(9000));
    ledger.Dispatch(1,0,0,at(10000));
    ledger.Dispatch(0,2,202,at(104300,300)); // migrate; no blocked gap charged
    ledger.Sample(0,at(106300,300));
    CHECK(ledger.Read(2,202,false,&value) && value==5000);
    CHECK(ledger.Read(2,202,true,&value) && value==5000);
    CHECK(!ledger.Read(2,202,false,nullptr));
    CHECK(!ledger.Read(16,202,false,&value));

    RuntimeAccounting<4,1> discontinuities;
    CHECK(discontinuities.Register(1,1,1,1));
    CHECK(discontinuities.Register(2,2,2,2));
    discontinuities.Dispatch(0,1,1,at(1000));
    CHECK(discontinuities.Sample(0,at(2000))==1000);
    CHECK(discontinuities.Sample(0,at(2100,500))==0); // delayed steal correction
    const auto firstClamp=discontinuities.Diagnostics().firstClamp;
    CHECK(firstClamp.sample==3 && firstClamp.previousWallNs==2000 && firstClamp.wallNs==2100);
    CHECK(firstClamp.previousStealNs==0 && firstClamp.stealNs==500);
    CHECK(firstClamp.serviceFloorNs==2000 && firstClamp.deficitNs==400);
    CHECK(firstClamp.cpu==0 && firstClamp.pid==1 && firstClamp.incarnation==1);
    CHECK(discontinuities.Sample(0,at(2400,500))==0);
    CHECK(discontinuities.Sample(0,at(2600,500))==100);
    CHECK(discontinuities.Diagnostics().clampedService==2);
    CHECK(discontinuities.Diagnostics().firstClamp.sample==firstClamp.sample);
    CHECK(discontinuities.Diagnostics().maxClamp.deficitNs==400);
    RuntimeAccounting<4,2> clampEvidence;
    CHECK(clampEvidence.Register(2,22,2,22));clampEvidence.Dispatch(1,2,22,at(10000));
    CHECK(clampEvidence.Sample(1,at(11000))==1000);
    CHECK(clampEvidence.Sample(1,at(11100,200))==0);
    CHECK(clampEvidence.Sample(1,at(11200,800))==0);
    auto clamp=clampEvidence.Diagnostics();
    CHECK(clamp.firstClamp.deficitNs==100 && clamp.maxClamp.deficitNs==600);
    CHECK(clamp.maxClamp.previousWallNs==11100 && clamp.maxClamp.previousStealNs==200);
    CHECK(clamp.maxClamp.wallNs==11200 && clamp.maxClamp.stealNs==800 && clamp.maxClamp.serviceFloorNs==11000);
    CHECK(clamp.maxClamp.cpu==1 && clamp.maxClamp.pid==2 && clamp.maxClamp.incarnation==22);
    // Diagnostics must not change the floor or manufacture catch-up service.
    CHECK(clampEvidence.Sample(1,at(11800,800))==0);
    CHECK(clampEvidence.Sample(1,at(11900,800))==100);
    discontinuities.Dispatch(0,2,2,{}); // unknown boundary is not charged to next task
    CHECK(discontinuities.Sample(0,at(10000,500))==0);
    CHECK(discontinuities.Sample(0,at(11000,500))==1000);
    CHECK(discontinuities.Sample(0,at(10999,500))==0);
    CHECK(discontinuities.Diagnostics().backwardsWall==1);
    CHECK(discontinuities.Diagnostics().maxWallRewindNs==1);
    CHECK(discontinuities.Diagnostics().backwardsCpuMask==1);
    CHECK(discontinuities.Diagnostics().lastBackwardsCpu==0);
    CHECK(discontinuities.Diagnostics().lastBackwardsPid==2);
    CHECK(discontinuities.Diagnostics().lastPreviousWallNs==11000);
    CHECK(discontinuities.Diagnostics().lastWallNs==10999);
    CHECK(discontinuities.Sample(0,at(12000,500))==0); // reestablish baseline
    CHECK(discontinuities.Sample(0,at(13000,499))==0);
    CHECK(discontinuities.Diagnostics().backwardsSteal==1);
    CHECK(discontinuities.Diagnostics().maxStealRewindNs==1);
    CHECK(discontinuities.Diagnostics().lastPreviousStealNs==500);
    CHECK(discontinuities.Diagnostics().lastStealNs==499);
    CHECK(discontinuities.Sample(0,at(14000,15000))==0);
    CHECK(discontinuities.Diagnostics().invalidSamples==2);
    CHECK(discontinuities.Read(1,1,false,&value) && value==1100);
    CHECK(discontinuities.Read(2,2,false,&value) && value==1000);

    RuntimeAccounting<4,2> rewindDetails;
    CHECK(rewindDetails.Register(1,1,1,1));
    rewindDetails.Dispatch(0,1,1,at(1000));
    rewindDetails.Sample(0,at(900));
    rewindDetails.Dispatch(1,1,1,at(100000));
    rewindDetails.Sample(1,at(99998));
    CHECK(rewindDetails.Diagnostics().maxWallRewindNs==100);
    CHECK(rewindDetails.Diagnostics().backwardsCpuMask==3);
    CHECK(rewindDetails.Diagnostics().lastBackwardsCpu==1);
    CHECK(rewindDetails.Diagnostics().lastBackwardsPid==1);
    CHECK(rewindDetails.Diagnostics().lastPreviousWallNs==100000);
    CHECK(rewindDetails.Diagnostics().lastWallNs==99998);
    CHECK(rewindDetails.Diagnostics().lastBackwardsSample==4);
    rewindDetails.Sample(1,at(~uint64_t(0)));
    rewindDetails.Sample(1,at(0));
    CHECK(rewindDetails.Diagnostics().maxWallRewindNs==~uint64_t(0));

    RuntimeAccounting<4,1> rebound;
    CHECK(rebound.Register(1,11,1,11));CHECK(rebound.Register(2,12,1,11));
    rebound.Dispatch(0,2,12,at(100));rebound.Sample(0,at(300));
    CHECK(rebound.Rebind(2,12,2,12)); // Brook nonleader exec becomes its own group
    rebound.Sample(0,at(500));
    CHECK(rebound.Read(1,11,true,&value) && value==200);
    CHECK(rebound.Read(2,12,true,&value) && value==400);
    CHECK(rebound.Read(2,12,false,&value) && value==400);
    CHECK(!rebound.Rebind(2,99,2,99));
    CHECK(rebound.Register(2,13,2,13)); // should not occur while running; detect identity error
    CHECK(rebound.Sample(0,at(600))==0 && rebound.Diagnostics().staleIdentity==1);

    RuntimeAccounting<4,2> saturation;
    CHECK(saturation.Register(1,1,1,1));CHECK(saturation.Register(2,2,1,1));
    saturation.Dispatch(0,1,1,at(0));saturation.Dispatch(1,2,2,at(0));
    saturation.Sample(0,at(~uint64_t(0)-5));saturation.Sample(1,at(100));
    CHECK(saturation.Read(1,1,true,&value) && value==~uint64_t(0));
    CHECK(saturation.Diagnostics().saturated==2); // global and group total

    // Independent elapsed-minus-steal oracle, distinct current owners on 8 CPUs.
    RuntimeAccounting<64,8> model;
    std::array<uint64_t,33> expected{}, groups{};
    std::array<uint16_t,8> current{};
    std::array<uint64_t,8> last{}, stolen{};
    for(unsigned pid=1;pid<=32;++pid) {
        const unsigned group=(pid-1)/4*4+1;
        CHECK(model.Register(pid,1000+pid,group,1000+group));
    }
    for(unsigned cpu=0;cpu<8;++cpu) model.Dispatch(cpu,0,0,at(0));
    uint64_t random=0x876543210, now=0;
    for(unsigned iteration=0;iteration<100000;++iteration) {
        random=random*6364136223846793005ULL+1;
        const unsigned cpu=(random>>32)%8;
        now+=1+(random>>16)%10000;
        const uint64_t elapsed=now-last[cpu], stealDelta=(random>>24)%(elapsed/5+1);
        stolen[cpu]+=stealDelta;last[cpu]=now;
        if(current[cpu]) {
            expected[current[cpu]]+=elapsed-stealDelta;
            groups[(current[cpu]-1)/4*4+1]+=elapsed-stealDelta;
        }
        if(random&1) model.Sample(cpu,at(now,stolen[cpu]));
        else {
            unsigned selected=(random>>8)%33;
            for(unsigned other=0;other<8;++other)
                if(other!=cpu && selected && current[other]==selected) selected=0;
            model.Dispatch(cpu,selected,selected?1000+selected:0,at(now,stolen[cpu]));
            current[cpu]=selected;
        }
    }
    for(unsigned pid=1;pid<=32;++pid) {
        CHECK(model.Read(pid,1000+pid,false,&value) && value==expected[pid]);
        CHECK(model.Read(pid,1000+pid,true,&value) && value==groups[pid]);
    }
    CHECK(model.Diagnostics().staleIdentity==0 && model.Diagnostics().invalidSamples==0);
    // The production try-claim never reserves an abandoned ticket and cannot
    // bypass an owner or previously queued waiter, including at uint32 wrap.
    volatile uint32_t next=5,serving=5;
    auto waitTicket=[&](uint32_t ticket){while(__atomic_load_n(&serving,__ATOMIC_ACQUIRE)!=ticket)std::this_thread::yield();};
    CHECK(brook::TicketTryClaim(&next,&serving,waitTicket) && next==6 && serving==5);
    CHECK(!brook::TicketTryClaim(&next,&serving,waitTicket) && next==6 && serving==5);
    next=8;CHECK(!brook::TicketTryClaim(&next,&serving,waitTicket) && next==8 && serving==5);
    next=serving=~uint32_t(0);
    CHECK(brook::TicketTryClaim(&next,&serving,waitTicket) && next==0 && serving==~uint32_t(0));
    __atomic_fetch_add(&serving,1,__ATOMIC_RELEASE);CHECK(serving==0 && brook::TicketTryClaim(&next,&serving,waitTicket));
    CHECK(brook::IrqAccountingMayDefer(true,1,true,false,true,false));
    CHECK(!brook::IrqAccountingMayDefer(false,1,true,false,true,false));
    CHECK(!brook::IrqAccountingMayDefer(true,0,true,false,true,false));
    CHECK(!brook::IrqAccountingMayDefer(true,1,false,false,true,false));
    CHECK(!brook::IrqAccountingMayDefer(true,1,true,true,true,false));
    CHECK(!brook::IrqAccountingMayDefer(true,1,true,false,false,false));
    CHECK(!brook::IrqAccountingMayDefer(true,1,true,false,true,true));
    next=serving=0;uint64_t protectedCount=0;std::atomic<unsigned> inside{0},overlap{0};
    std::vector<std::thread> lockers;
    for(unsigned worker=0;worker<4;++worker)lockers.emplace_back([&,worker]{
        for(unsigned i=0;i<5000;++i){
            if(worker<2){
                const auto ticket=__atomic_fetch_add(&next,1,__ATOMIC_RELAXED);
                while(__atomic_load_n(&serving,__ATOMIC_ACQUIRE)!=ticket)std::this_thread::yield();
            }else while(!brook::TicketTryClaim(&next,&serving,waitTicket))std::this_thread::yield();
            if(inside.fetch_add(1)!=0)++overlap;
            ++protectedCount;
            inside.fetch_sub(1);
            __atomic_fetch_add(&serving,1,__ATOMIC_RELEASE);
        }
    });
    for(auto& thread:lockers)thread.join();CHECK(protectedCount==20000 && !inside && !overlap && next==serving);
    // Inject an entire counter cycle between serving read and CAS, without
    // doing billions of operations. A stale successful reservation must not
    // enter until the actual owner releases, and must release exactly once.
    next=serving=7;std::atomic<bool> waiting{false},finished{false},enteredEarly{false},ownerReleased{false};
    std::atomic<unsigned> releases{0};
    std::thread wrapped([&]{
        const bool got=brook::TicketTryClaim(&next,&serving,[&](uint32_t ticket){
            waiting.store(true,std::memory_order_release);waitTicket(ticket);
        },[&]{__atomic_store_n(&serving,6,__ATOMIC_RELEASE);});
        if(got){
            if(!ownerReleased.load(std::memory_order_acquire))enteredEarly=true;
            __atomic_fetch_add(&serving,1,__ATOMIC_RELEASE);++releases;
        }
        finished.store(true,std::memory_order_release);
    });
    while(!waiting.load(std::memory_order_acquire) && !finished.load(std::memory_order_acquire))std::this_thread::yield();
    CHECK(waiting && !finished && !enteredEarly && __atomic_load_n(&next,__ATOMIC_ACQUIRE)==8);
    ownerReleased.store(true,std::memory_order_release);__atomic_fetch_add(&serving,1,__ATOMIC_RELEASE);
    wrapped.join();CHECK(!enteredEarly && releases==1 && next==8 && serving==8);
    CHECK(brook::TicketTryClaim(&next,&serving,waitTicket));__atomic_fetch_add(&serving,1,__ATOMIC_RELEASE);CHECK(next==9 && serving==9);
    // Identical SMP ownership/migration events with eager versus deferred timer
    // samples. Dispatch/query boundaries must charge exactly once to the old
    // owner, preserving task and group clocks despite arbitrary skipped ticks.
    RuntimeAccounting<40,8> eager,deferred;
    for(unsigned pid=1;pid<=32;++pid){
        CHECK(eager.Register(pid,100+pid,(pid-1)/4*4+1,100+(pid-1)/4*4+1));
        CHECK(deferred.Register(pid,100+pid,(pid-1)/4*4+1,100+(pid-1)/4*4+1));
    }
    for(unsigned cpu=0;cpu<8;++cpu){eager.Dispatch(cpu,0,0,at(0));deferred.Dispatch(cpu,0,0,at(0));}
    uint64_t wall[8]{},steal[8]{};unsigned owner[8]{};
    uint32_t rng=0x827abcd;
    for(unsigned event=0;event<40000;++event){
        rng=rng*1664525+1013904223;unsigned cpu=rng%8;wall[cpu]+=1000+(rng>>8)%10000;steal[cpu]+=(rng>>16)%100;
        if((rng>>4)%7==0){
            unsigned selected=(rng>>12)%33;
            for(unsigned other=0;other<8;++other)if(other!=cpu && selected && owner[other]==selected)selected=0;
            eager.Dispatch(cpu,selected,selected?100+selected:0,at(wall[cpu],steal[cpu]));
            deferred.Dispatch(cpu,selected,selected?100+selected:0,at(wall[cpu],steal[cpu]));owner[cpu]=selected;
        }else {
            eager.Sample(cpu,at(wall[cpu],steal[cpu]));
            if((rng>>20)%11==0)deferred.Sample(cpu,at(wall[cpu],steal[cpu]));
        }
    }
    for(unsigned cpu=0;cpu<8;++cpu){eager.Dispatch(cpu,0,0,at(wall[cpu],steal[cpu]));deferred.Dispatch(cpu,0,0,at(wall[cpu],steal[cpu]));}
    for(unsigned pid=1;pid<=32;++pid)for(bool group:{false,true}){
        uint64_t other=0;CHECK(eager.Read(pid,100+pid,group,&value) && deferred.Read(pid,100+pid,group,&other) && value==other);
    }
    CHECK(eager.Diagnostics().chargedNs==deferred.Diagnostics().chargedNs && eager.Diagnostics().idleNs==deferred.Diagnostics().idleNs);
    CHECK(!deferred.Diagnostics().invalidSamples && !deferred.Diagnostics().staleIdentity);
    RuntimeAccounting<4,1> skipped;CHECK(skipped.Register(1,1,1,1));CHECK(skipped.Register(2,2,2,2));
    skipped.Dispatch(0,1,1,at(100,10)); // several failed tries do not call Sample
    CHECK(skipped.Sample(0,at(1000,110))==800); // CPU-clock/block boundary catch-up
    CHECK(skipped.Sample(0,at(1000,110))==0); // no double charge at dispatch
    skipped.Dispatch(0,2,2,at(1100,110));CHECK(skipped.Read(1,1,false,&value) && value==900);
    CHECK(skipped.Sample(0,at(1400,210))==200);CHECK(skipped.Read(2,2,false,&value) && value==200);
    skipped.Sample(0,{}); // failed later sample discards unknown deferred gap
    skipped.Dispatch(0,1,1,at(10000,210));CHECK(skipped.Read(2,2,false,&value) && value==200);
    // The observer must preserve all ledger results, including failures and
    // corrections across ownership changes. It reuses exactly the input pair.
    {
        RuntimeAccounting<8,2> plain, watched;
        brook::RuntimeHistory<2> history;
        watched.SetHistory(&history); // attached but disabled is also inert
        for(unsigned pid=1;pid<=4;++pid){
            CHECK(plain.Register(pid,100+pid,pid,100+pid));
            CHECK(watched.Register(pid,100+pid,pid,100+pid));
        }
        auto sampleBoth=[&](unsigned cpu,brook::RuntimeSample p){
            auto a=plain.Sample(cpu,p),b=watched.Sample(cpu,p);CHECK(a==b);return b;
        };
        for(unsigned cpu=0;cpu<2;++cpu){plain.Dispatch(cpu,cpu+1,101+cpu,at(1000,100));watched.Dispatch(cpu,cpu+1,101+cpu,at(1000,100));}
        brook::RuntimeHistorySnapshot h{};
        CHECK(history.Read(0,&h) && !h.enabled && !h.count && !h.epoch);
        CHECK(!history.Start(0) && !history.Start(3));
        CHECK(history.Start(2));
        sampleBoth(0,at(123792817503ULL,118281443));
        CHECK(sampleBoth(0,at(123792870751ULL,118339578))==0);
        CHECK(history.Read(0,&h) && h.triggered && h.triggerCpu==0 && !h.caughtUp);
        CHECK(h.triggerFloorNs==123674536060ULL);
        const auto triggeredEpoch=h.epoch,triggeredSequence=h.triggerSequence;
        CHECK(history.Start(2)); // duplicate on preserves the positive event
        CHECK(history.Read(0,&h) && h.epoch==triggeredEpoch && h.triggerSequence==triggeredSequence);
        plain.SetOwner(0,3,103);watched.SetOwner(0,3,103);
        CHECK(sampleBoth(0,at(123792970751ULL,118339578))==95113);
        CHECK(history.Read(0,&h) && h.caughtUp && h.catchupSequence>h.triggerSequence);
        bool sawOwner=false,sawCorrection=false;
        for(unsigned i=0;i<h.count;++i){const auto& e=h.entries[i];
            if(e.kind==brook::RuntimeHistoryKind::Owner && e.pidBefore==1 && e.pidAfter==3){
                sawOwner=true;CHECK(e.wallNs==123792870751ULL && !e.valid);
            }
            if(e.sequence==h.catchupSequence){sawCorrection=true;CHECK(e.pidBefore==3 && e.chargedNs==95113);}
        }
        CHECK(sawOwner && sawCorrection);
        uint64_t wall[2]{123792970751ULL,1000},steal[2]{118339578,100};
        for(unsigned i=0;i<24;++i)sampleBoth(0,at(wall[0]+=1000,steal[0]));
        CHECK(history.Read(0,&h) && h.frozen && !h.allFrozen && h.omittedAfterFreeze>0);
        for(unsigned i=0;i<16;++i)sampleBoth(1,at(wall[1]+=1000,steal[1]));
        CHECK(history.Read(0,&h) && h.allFrozen && h.caughtUp && !history.Recording());
        const auto frozenSequence=h.sequence, frozenOmitted=h.omittedAfterFreeze;
        CHECK(h.count==18 && h.postEvents==16 && h.entries[1].kind==brook::RuntimeHistoryKind::Clamp);
        sampleBoth(0,at(wall[0]+=1000,steal[0]));
        CHECK(history.Read(0,&h) && h.sequence==frozenSequence && h.omittedAfterFreeze==frozenOmitted);
        h.entries[0].sequence=0x1234;CHECK(history.Read(0,&h,false) && h.count==18 && h.entries[0].sequence==0x1234);
        CHECK(history.Reset(2));
        for(unsigned i=0;i<96;++i)sampleBoth(0,at(wall[0]+=1000,steal[0]));
        CHECK(history.Read(0,&h) && h.count==64 && h.prefixOverwritten==32 && !h.triggered);
        for(unsigned i=1;i<h.count;++i)CHECK(h.entries[i-1].sequence<h.entries[i].sequence);
        CHECK(h.epoch>triggeredEpoch);
        steal[0]+=1100;sampleBoth(0,at(wall[0]+=1000,steal[0])); // new clamp, despite nonzero lifetime clamp counter
        CHECK(history.Read(0,&h) && h.triggered && !h.caughtUp);
        sampleBoth(0,{}); // unknown interval must prevent a false catch-up claim
        for(unsigned cpu=0;cpu<2;++cpu)for(unsigned i=0;i<20;++i)sampleBoth(cpu,at(wall[cpu]+=1000,steal[cpu]));
        CHECK(history.Read(0,&h) && h.allFrozen && h.baselineLost && !h.caughtUp);
        CHECK(h.count==64 && h.postEvents==16 && h.entries[47].sequence==h.triggerSequence && h.entries[47].kind==brook::RuntimeHistoryKind::Clamp);
        history.Stop();CHECK(!history.Recording() && !history.Enabled());
        CHECK(!history.Read(2,&h) && !history.Read(0,nullptr));
        // Exercise observer reset/stop with invalid/backwards pairs, new owners
        // and PID generations. Comparing all diagnostic fields avoids treating
        // agreement of just the successful charges as sufficient.
        auto equalEvidence=[&](const brook::RuntimeClampEvidence& a,const brook::RuntimeClampEvidence& b){
#define EQUAL_EVIDENCE(field) CHECK(a.field==b.field)
            EQUAL_EVIDENCE(sample);EQUAL_EVIDENCE(previousWallNs);EQUAL_EVIDENCE(wallNs);
            EQUAL_EVIDENCE(previousStealNs);EQUAL_EVIDENCE(stealNs);EQUAL_EVIDENCE(serviceFloorNs);
            EQUAL_EVIDENCE(deficitNs);EQUAL_EVIDENCE(incarnation);EQUAL_EVIDENCE(cpu);EQUAL_EVIDENCE(pid);
#undef EQUAL_EVIDENCE
        };
        uint32_t random=0x46b8c123;uint64_t generation[5]{0,101,102,103,104};
        for(unsigned i=0;i<40000;++i){
            random=random*1664525+1013904223;unsigned cpu=random%2;
            if(i%251==0)CHECK(history.Reset(2));
            if(i%997==0)history.Stop();else if(i%997==1)CHECK(history.Start(2));
            if(i%683==0){unsigned pid=(random>>10)%4+1;generation[pid]+=1000;CHECK(plain.Register(pid,generation[pid],pid,generation[pid]));CHECK(watched.Register(pid,generation[pid],pid,generation[pid]));}
            auto p=at(wall[cpu]+=1000+(random>>8)%10000,steal[cpu]+=(random>>16)%100);
            if(i%101==0)p={};
            if(i%127==0)p.wallNs=1;
            if(i%137==0)p.stealNs=0;
            sampleBoth(cpu,p);
            if(i%7==0){unsigned pid=(random>>20)%5;plain.SetOwner(cpu,pid,generation[pid]);watched.SetOwner(cpu,pid,generation[pid]);}
            const auto& a=plain.Diagnostics();const auto& b=watched.Diagnostics();
#define EQUAL_DIAGNOSTIC(field) CHECK(a.field==b.field)
            EQUAL_DIAGNOSTIC(samples);EQUAL_DIAGNOSTIC(chargedNs);EQUAL_DIAGNOSTIC(idleNs);
            EQUAL_DIAGNOSTIC(invalidSamples);EQUAL_DIAGNOSTIC(backwardsWall);EQUAL_DIAGNOSTIC(backwardsSteal);
            EQUAL_DIAGNOSTIC(clampedService);EQUAL_DIAGNOSTIC(staleIdentity);EQUAL_DIAGNOSTIC(saturated);
            EQUAL_DIAGNOSTIC(observedStealNs);EQUAL_DIAGNOSTIC(maxWallRewindNs);EQUAL_DIAGNOSTIC(maxStealRewindNs);
            EQUAL_DIAGNOSTIC(backwardsCpuMask);EQUAL_DIAGNOSTIC(lastBackwardsSample);
            EQUAL_DIAGNOSTIC(lastPreviousWallNs);EQUAL_DIAGNOSTIC(lastWallNs);
            EQUAL_DIAGNOSTIC(lastPreviousStealNs);EQUAL_DIAGNOSTIC(lastStealNs);
            EQUAL_DIAGNOSTIC(lastBackwardsCpu);EQUAL_DIAGNOSTIC(lastBackwardsPid);
#undef EQUAL_DIAGNOSTIC
            equalEvidence(a.firstClamp,b.firstClamp);equalEvidence(a.maxClamp,b.maxClamp);
        }
        for(unsigned pid=1;pid<=4;++pid)for(bool group:{false,true}){
            uint64_t a=0,b=0;CHECK(plain.Read(pid,generation[pid],group,&a) && watched.Read(pid,generation[pid],group,&b) && a==b);
        }
        // Prefix/tail exhaustion is explicitly incomplete when debt has not
        // caught up. No observer event may reset the floor to manufacture time.
        RuntimeAccounting<4,1> debt;brook::RuntimeHistory<1> debtHistory;debt.SetHistory(&debtHistory);
        CHECK(debt.Register(1,1,1,1));debt.Dispatch(0,1,1,at(1000,100));CHECK(debtHistory.Start(1));
        debt.Sample(0,at(1100,300));
        for(unsigned i=1;i<=16;++i)debt.Sample(0,at(1100+i,300+2*i));
        CHECK(debtHistory.Read(0,&h) && h.allFrozen && !h.caughtUp && !h.baselineLost);
        CHECK(debt.Read(1,1,false,&value) && value==0);
    }
    std::printf("RUNTIME_ACCOUNTING %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
