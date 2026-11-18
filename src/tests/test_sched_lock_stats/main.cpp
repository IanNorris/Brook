// Scheduler-lock statistics and unfair ticket claims (sched_lock_stats.h,
// sched_lock_claim.h): bucket math, epoch reset, site table, aggregation,
// and mutual exclusion/progress when unfair, FIFO and try claims mix.
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>
#include "../../kernel/src/sched_lock_stats.h"
#include "../../kernel/src/sched_lock_claim.h"
#include "../../kernel/src/irq_accounting.h"
#include "../../kernel/src/sched_lock_owner.h"
using namespace brook;
static std::atomic<unsigned> failures{0};
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"lock stats line %d: %s\n",__LINE__,#x);++failures;}}while(0)

static void WaitTicket(volatile uint32_t* serving,uint32_t t){
    while(__atomic_load_n(serving,__ATOMIC_ACQUIRE)!=t)std::this_thread::yield();
}

int main(){
    CHECK(LockStatBucket(0)==0 && LockStatBucket(127)==0 && LockStatBucket(128)==1 && LockStatBucket(255)==1);
    CHECK(LockStatBucket(256)==2 && LockStatBucket(~uint64_t(0))==LockStatBuckets-1);

    auto stats=std::make_unique<SchedLockStats>();auto merged=std::make_unique<SchedLockStats::Merged>();
    CHECK(!stats->Enabled());stats->Control(1,false);CHECK(stats->Enabled());
    static const char siteA[]="SiteA",siteB[]="SiteB";
    LockStatHold hold{};
    // Uncontended: no wait/handoff contribution; hold measured.
    stats->Acquired(0,0,siteA,10,false,100,0,150,0,0,&hold);CHECK(hold.valid && hold.cpu==0);
    stats->Released(0,0,&hold,1150,5);CHECK(!hold.valid);
    // Contended with steal and a predecessor release: wait 1000, handoff 300.
    stats->Acquired(1,0,siteA,10,true,1000,10,2000,40,1700,&hold);
    stats->Released(1,0,&hold,2100,40);
    stats->Acquired(1,1,siteB,20,true,0,0,500,0,0,&hold);stats->Released(1,1,&hold,700,0);
    stats->Released(1,1,&hold,9999,0); // double release of an invalid hold is ignored
    stats->Read(4,merged.get());
    const auto& r=merged->lock[0];
    CHECK(merged->cpusInEpoch==2);
    CHECK(r.acquisitions==2 && r.contended==1 && r.waitCycles==1000 && r.maxWaitCycles==1000);
    CHECK(r.holdCycles==1100 && r.maxHoldCycles==1000 && r.holdStealNs==5 && r.stolenHolds==1);
    CHECK(r.waitStealNs==30 && r.handoffs==1 && r.handoffCycles==300);
    CHECK(r.holdHist[LockStatBucket(1000)]==1 && r.holdHist[LockStatBucket(100)]==1 && r.waitHist[LockStatBucket(1000)]==1);
    CHECK(merged->lock[1].acquisitions==1 && merged->lock[1].handoffs==0 && merged->lock[1].holdCycles==200);
    CHECK(merged->sites==2 && merged->site[0].caller==siteA && merged->site[0].line==10 && merged->site[0].acquisitions==2);
    CHECK(merged->site[0].holdCycles==1100 && merged->site[1].caller==siteB && merged->site[1].lock==1);

    // A hold that spans a reset is dropped; rows clear lazily on next use.
    stats->Acquired(2,0,siteA,10,false,0,0,10,0,0,&hold);
    stats->Control(-1,true);
    stats->Released(2,0,&hold,1000000,0);
    stats->Read(4,merged.get());CHECK(merged->cpusInEpoch==0 && merged->sites==0);
    stats->Acquired(1,2,siteB,30,false,0,0,1,0,0,&hold);stats->Released(1,2,&hold,2,0);
    stats->Read(4,merged.get());
    CHECK(merged->cpusInEpoch==1 && merged->lock[0].acquisitions==0 && merged->lock[2].acquisitions==1 && merged->sites==1);
    // Same caller at a different line is a distinct site; table overflow counts.
    for(uint32_t line=0;line<LockStatSiteSlots+5;++line){stats->Acquired(3,0,siteA,line,false,0,0,1,0,0,&hold);stats->Released(3,0,&hold,2,0);}
    stats->Read(4,merged.get());CHECK(merged->siteOverflow==5);
    // Sites from different CPUs merge.
    stats->Acquired(1,2,siteB,30,false,0,0,1,0,0,&hold);stats->Released(1,2,&hold,2,0);
    stats->Acquired(0,2,siteB,30,false,0,0,1,0,0,&hold);stats->Released(0,2,&hold,2,0);
    stats->Read(4,merged.get());unsigned found=0;
    for(unsigned i=0;i<merged->sites;++i)if(merged->site[i].caller==siteB && merged->site[i].line==30){++found;CHECK(merged->site[i].acquisitions==3);}
    CHECK(found==1);

    // Unfair claim: immediate when free; falls back to a FIFO ticket after the budget.
    {
        volatile uint32_t next=5,serving=5;bool contended=false,fell=false;unsigned spins=0;
        CHECK(TicketUnfairAcquire(&next,&serving,4,[&](uint64_t){++spins;},[&](uint32_t t){WaitTicket(&serving,t);},&contended,&fell)==5);
        CHECK(!contended && !fell && next==6 && spins==0);
        // Held: a second claim spins budget-1 times, then queues ticket 6.
        std::thread releaser([&]{while(__atomic_load_n(&next,__ATOMIC_ACQUIRE)!=7)std::this_thread::yield();__atomic_fetch_add(&serving,1,__ATOMIC_RELEASE);});
        contended=false;
        CHECK(TicketUnfairAcquire(&next,&serving,4,[&](uint64_t){++spins;},[&](uint32_t t){WaitTicket(&serving,t);},&contended,&fell)==6);
        releaser.join();CHECK(contended && fell && spins==3 && serving==6);
        // Outstanding FIFO tickets block unfair claims (no queue jumping).
        next=10;serving=8;contended=false;fell=false;spins=0;
        std::thread drain([&]{while(__atomic_load_n(&spins,__ATOMIC_RELAXED)<100)std::this_thread::yield();
            __atomic_fetch_add(&serving,1,__ATOMIC_RELEASE);__atomic_fetch_add(&serving,1,__ATOMIC_RELEASE);});
        CHECK(TicketUnfairAcquire(&next,&serving,~uint64_t(0),[&](uint64_t){__atomic_fetch_add(&spins,1u,__ATOMIC_RELAXED);std::this_thread::yield();},
            [&](uint32_t t){WaitTicket(&serving,t);},&contended,&fell)==10);
        drain.join();CHECK(contended && !fell && next==11 && serving==10);
    }

    // Concurrency: unfair, FIFO and try claimers, owner publication, uint32 wrap.
    {
        ReadyLockOwnerObservation owner;owner.Mode(true);
        volatile uint32_t next=UINT32_MAX-100,serving=UINT32_MAX-100;
        std::atomic<unsigned> active{0};uint64_t value=0;std::atomic<uint64_t> fallbacks{0};
        std::vector<std::thread> threads;
        for(unsigned cpu=0;cpu<8;++cpu)threads.emplace_back([&,cpu]{
            for(unsigned i=0;i<4000;++i){
                uint32_t ticket=0;bool claimed=false;
                const unsigned kind=(i+cpu)%4;
                if(kind==0)claimed=TicketTryClaim(&next,&serving,[&](uint32_t t){WaitTicket(&serving,t);},TicketNoHook{},[&](uint32_t t){ticket=t;});
                if(!claimed && kind==1){ticket=__atomic_fetch_add(&next,1,__ATOMIC_RELAXED);WaitTicket(&serving,ticket);claimed=true;}
                if(!claimed){bool contended=false,fell=false;
                    ticket=TicketUnfairAcquire(&next,&serving,256,[](uint64_t){std::this_thread::yield();},[&](uint32_t t){WaitTicket(&serving,t);},&contended,&fell);
                    if(fell)fallbacks.fetch_add(1);}
                CHECK(__atomic_load_n(&serving,__ATOMIC_ACQUIRE)==ticket);
                owner.Acquired(cpu,ticket);CHECK(active.fetch_add(1)==0);++value;CHECK(active.fetch_sub(1)==1);
                owner.Releasing(cpu,ticket);__atomic_fetch_add(&serving,1,__ATOMIC_RELEASE);
            }
        });
        for(auto& t:threads)t.join();
        ReadyLockOwnerSnapshot snap{};owner.Read(8,&snap);
        CHECK(value==32000 && next==serving && !snap.owner);
        for(unsigned c=0;c<8;++c)CHECK(!snap.cpu[c].errors);
        std::printf("unfair fallbacks under contention: %llu\n",(unsigned long long)fallbacks.load());
    }

    // Unfair-wait owner hint: no ticket, still bounded to one yield.
    {
        ReadyLockOwnerObservation o;o.Mode(true);CHECK(o.YieldMode(true));o.Acquired(1,10);
        unsigned yields=0;bool spent=false;
        auto pre=[](unsigned,uint8_t* out){*out=1;return true;};
        auto perform=[&](unsigned cpu,int64_t* ret){CHECK(cpu==1);++yields;*ret=0;return true;};
        for(int i=0;i<5;++i)o.ObserveUnfairWait(2,spent,[]{return 10u;},pre,perform,[]{return uint64_t(0);});
        CHECK(spent && yields==1);
        ReadyLockOwnerSnapshot snap{};o.Read(3,&snap);
        CHECK(snap.cpu[2].hintAttempted==1 && snap.cpu[2].hintBudgetExhausted==4 && !snap.cpu[2].kind[unsigned(OwnerWaitObservation::AlreadyGranted)]);
        o.Releasing(1,10);
    }
    std::printf("SCHED_LOCK_STATS %s\n",failures.load()?"FAIL":"PASS");return failures?1:0;
}
