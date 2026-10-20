#include <array>
#include <cstdio>
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
    CHECK(pair.valid && pair.wallNs==200 && pair.stealNs==40 && wallReads==2 && stealReads==4);
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
    std::printf("RUNTIME_ACCOUNTING %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
