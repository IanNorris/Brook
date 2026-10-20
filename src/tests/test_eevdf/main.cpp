#include <cstdio>
#include <cstdint>
#include <array>
#include <algorithm>
#include <dlfcn.h>
#include <cstdlib>
#include "../../kernel/src/eevdf_policy.h"
#include "../../kernel/src/sched_restore.h"
namespace {
int failures=0;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"EEVDF line %d: %s\n",__LINE__,#x);++failures;}}while(0)
}
int main(int argc,char** argv){
    using namespace brook;
    uint64_t random=0x1234587;
    for(unsigned n=0;n<20000;++n){
        random=random*6364136223846793005ULL+1;const uint64_t divisor=(random|1);
        random=random*6364136223846793005ULL+1;const uint64_t high=random%divisor;
        random=random*6364136223846793005ULL+1;const auto numerator=(static_cast<unsigned __int128>(high)<<64)|random;
        const auto actual=EevdfDivide(numerator,divisor);
        CHECK(!actual.overflow && actual.quotient==numerator/divisor && actual.remainder==numerator%divisor);
    }
    CHECK(EevdfDivide(static_cast<unsigned __int128>(1)<<64,1).overflow);
    CHECK(EevdfDivide(1,0).overflow);
    if(argc==2){
        void* library=dlopen(argv[1],RTLD_NOW);CHECK(library);
        if(library){
            auto get=reinterpret_cast<const SchedOps*(*)()>(dlsym(library,"GetSchedOps"));CHECK(get);
            if(get){
                const SchedOps* ops=get();CHECK(SchedOpsUsable(ops));
                if(SchedOpsUsable(ops)){
                    void* storage=std::calloc(1,ops->stateSize);CHECK(storage);
                    if(storage){
                        ops->Init(storage);ops->InitProcess(storage,1,2);ops->InitProcess(storage,2,0);
                        ops->Enqueue(storage,1);ops->Enqueue(storage,2);CHECK(ops->PickNext(storage)==2);
                        ops->AccountService(storage,2,1000000);CHECK(ops->ShouldPreempt(storage,2,0));
                        ops->Remove(storage,2);ops->SetRunnable(storage,2,false);CHECK(ops->PickNext(storage)==1);
                        ops->Init(storage);
                        CHECK(!SchedRestoreProcess(ops,storage,1,2,false,false,0,true));
                        CHECK(!SchedRestoreProcess(ops,storage,2,2,false,false,1,false));
                        CHECK(SchedRestoreProcess(ops,storage,3,2,false,true,-1));
                        auto* actual=static_cast<EevdfPolicy*>(storage);
                        CHECK(actual->Inspect(1).runnable && !actual->Inspect(1).queued);
                        CHECK(!actual->Inspect(2).runnable && !actual->Inspect(2).queued);
                        CHECK(ops->PickNext(storage)==3 && ops->PickNext(storage)==SCHED_PID_NONE);
                        std::free(storage);
                    }
                }
            }
            dlclose(library);
        }
    }
    EevdfPolicy p;p.Init();
    CHECK(p.PickNext()==SCHED_PID_NONE && p.ReadyCount()==0);
    p.AccountService(17,1234); // final old-epoch service, no terminated entity resurrection
    CHECK(p.Statistics().serviceNs==1234 && p.Statistics().inactiveServiceNs==1234);
    CHECK(!p.Inspect(17).active && p.ReadyCount()==0);
    p.Init();
    p.InitProcess(0,0);p.Enqueue(0);p.InitProcess(SCHED_PID_NONE,0);p.Enqueue(SCHED_PID_NONE);
    CHECK(p.ReadyCount()==0);
    p.InitProcess(1,2);p.InitProcess(2,0);p.Enqueue(1);p.Enqueue(2);
    CHECK(p.PickNext()==2); // equal lag; shorter virtual deadline first
    CHECK(p.Inspect(2).runnable && !p.Inspect(2).queued);
    CHECK(p.PickNext()==1); // spare CPU remains work conserving
    p.AccountService(2,1000000);p.Enqueue(2);
    CHECK(p.Inspect(2).remainingNs==1000000); // exhausted request renewed at enqueue
    CHECK(p.PickNext()==2);
    p.Init();p.InitProcess(1,2);p.InitProcess(2,2);p.Enqueue(1);p.Enqueue(2);
    CHECK(p.PickNext()==1);
    p.AccountService(1,1500000);CHECK(p.ShouldPreempt(1,0));p.VoluntaryYield(1);
    CHECK(p.Inspect(1).remainingNs==2500000);
    p.Enqueue(1);CHECK(p.PickNext()==2);
    p.AccountService(2,2000000);p.Remove(2);p.SetRunnable(2,false);
    const auto debt=p.Inspect(2);CHECK(debt.deferred && debt.savedLag<0);
    p.SetRunnable(2,true);p.Enqueue(2);
    CHECK(p.Inspect(2).virtualRuntime==debt.virtualRuntime); // short sleep cannot erase debt
    CHECK(p.Inspect(2).remainingNs==2000000);
    CHECK(!p.InitialWeight(2,2048));
    p.InitProcess(2,2);CHECK(p.InitialWeight(2,2048)); // PID reuse resets membership/debt
    CHECK(!p.Inspect(2).queued && !p.Inspect(2).deferred);

    p.Init();p.InitProcess(1,2);CHECK(p.InitialWeight(1,1u<<20));p.Enqueue(1);CHECK(p.PickNext()==1);
    for(unsigned n=0;n<100000;++n)p.AccountService(1,1);
    CHECK(p.Inspect(1).virtualRuntime==int64_t(100000ULL*1024/(1u<<20))); // fractional service is not lost per query
    CHECK(p.Inspect(1).remainingNs==3900000);

    // Weighted one-CPU service: each task is always runnable, initial weights
    // vary. Compare normalized physical service against an independent ideal.
    p.Init();std::array<uint64_t,5> service{};
    const uint32_t weights[]={0,512,1024,2048,4096};
    for(uint16_t pid=1;pid<=4;++pid){p.InitProcess(pid,2);CHECK(p.InitialWeight(pid,weights[pid]));p.Enqueue(pid);}
    uint16_t current=p.PickNext();
    for(unsigned step=0;step<100000;++step){
        CHECK(current>0 && current<=4);if(!current || current>4)break;
        p.AccountService(current,100000);service[current]+=100000;
        if(p.ShouldPreempt(current,step)){
            p.TimesliceExpired(current);p.Enqueue(current);current=p.PickNext();
        }
    }
    uint64_t total=0;for(unsigned pid=1;pid<=4;++pid)total+=service[pid];
    for(unsigned pid=1;pid<=4;++pid){
        const uint64_t ideal=total*weights[pid]/7680;
        CHECK(service[pid]>ideal-8000000 && service[pid]<ideal+8000000);
    }

    // Eight actual owners in the model, 24 runnable tasks. Kernel mechanism
    // ownership is separate, but a policy must never return an already-picked
    // task until the caller explicitly publishes its off-CPU requeue.
    p.Init();std::array<uint16_t,8> owners{};std::array<uint64_t,25> work{};
    for(uint16_t pid=1;pid<=24;++pid){p.InitProcess(pid,2);p.Enqueue(pid);}
    for(auto& owner:owners)owner=p.PickNext();
    for(unsigned step=0;step<100000;++step){
        const unsigned cpu=step%8;const uint16_t running=owners[cpu];
        CHECK(running>0 && running<=24);if(!running || running>24)break;
        p.AccountService(running,100000);work[running]+=100000;
        if(p.ShouldPreempt(running,0)){
            p.TimesliceExpired(running);
            const uint16_t next=p.PickNext();CHECK(next!=SCHED_PID_NONE);
            for(unsigned other=0;other<8;++other)CHECK(next!=owners[other]);
            p.Enqueue(running);owners[cpu]=next;
        }
        if((step%17)==0)p.Tick(step);
    }
    const auto [low,high]=std::minmax_element(work.begin()+1,work.end());
    const uint64_t equalSpread=*high-*low;
    CHECK(*low>0 && equalSpread<=16000000);
    CHECK(p.ReadyCount()==16);
    CHECK(p.Statistics().saturations==0);
    SchedDebugInfo info{};p.DebugDump(1,&info);CHECK(info.readyCount==info.listLen);

    // Repeated voluntary block/wake cannot obtain fresh CPU requests or erase
    // negative lag while a competing peer remains runnable.
    p.Init();p.InitProcess(1,2);p.InitProcess(2,2);p.Enqueue(1);p.Enqueue(2);
    current=p.PickNext();std::array<uint64_t,3> yielded{};
    for(unsigned step=0;step<40000;++step){
        p.AccountService(current,100000);yielded[current]+=100000;
        if(current==1){
            p.Remove(1);p.SetRunnable(1,false);p.VoluntaryYield(1);p.Enqueue(1);
        }else p.Enqueue(2);
        current=p.PickNext();CHECK(current==1 || current==2);
    }
    CHECK(yielded[1]<yielded[2]+16000000 && yielded[2]<yielded[1]+16000000);

    // Late outgoing service after remote block publication still creates debt.
    p.Init();p.InitProcess(1,2);p.InitProcess(2,2);p.Enqueue(1);p.Enqueue(2);
    CHECK(p.PickNext()==1);p.SetRunnable(1,false);p.AccountService(1,1000000);p.Enqueue(1);
    CHECK(p.Inspect(1).virtualRuntime-p.VirtualTime()==1000000);
    CHECK(p.Inspect(1).remainingNs==3000000);
    // Deferred negative debt decays through competitors' actual virtual work.
    p.Remove(1);p.SetRunnable(1,false);CHECK(p.Inspect(1).deferred);
    p.AccountService(2,10000000);p.Tick(1);
    CHECK(!p.Inspect(1).deferred);p.Enqueue(1);
    CHECK(p.Inspect(1).virtualRuntime==p.Inspect(2).virtualRuntime);
    // Exceptional positive sleeper credit is bounded rather than hoarded.
    p.Init();p.InitProcess(1,2);p.InitProcess(2,2);p.Enqueue(1);p.Enqueue(2);
    p.AccountService(1,20000000);p.Remove(2);p.SetRunnable(2,false);p.Enqueue(2);
    CHECK(p.VirtualTime()-p.Inspect(2).virtualRuntime==8000000);

    // Unequal weights on eight CPUs, with demand feasible below one CPU per
    // task: compare physical service against aggregate weighted fair shares.
    p.Init();owners.fill(0);work.fill(0);uint64_t weightSum=0;
    for(uint16_t pid=1;pid<=24;++pid){
        p.InitProcess(pid,2);const uint32_t weight=512u<<((pid-1)%4);
        CHECK(p.InitialWeight(pid,weight));weightSum+=weight;p.Enqueue(pid);
    }
    for(auto& owner:owners)owner=p.PickNext();
    for(unsigned step=0;step<100000;++step){
        const unsigned cpu=step%8;const uint16_t running=owners[cpu];
        CHECK(running>0 && running<=24);if(!running || running>24)break;
        p.AccountService(running,100000);work[running]+=100000;
        if(p.ShouldPreempt(running,0)){
            const auto next=p.PickNext();CHECK(next!=SCHED_PID_NONE);
            for(const auto owner:owners)CHECK(owner!=next);
            p.Enqueue(running);owners[cpu]=next;
        }
    }
    uint64_t weightedSmpError=0;
    for(unsigned pid=1;pid<=24;++pid){
        const uint64_t ideal=10000000000ULL*(512u<<((pid-1)%4))/weightSum;
        weightedSmpError=std::max(weightedSmpError,work[pid]>ideal?work[pid]-ideal:ideal-work[pid]);
        CHECK(work[pid]+24000000>=ideal && work[pid]<=ideal+24000000);
    }

    p.Init();p.InitProcess(1,2);p.Enqueue(1);CHECK(p.PickNext()==1);
    p.AccountService(1,1ULL<<51);CHECK(p.Statistics().rebases>0);
    CHECK(p.Inspect(1).virtualRuntime==0);
    p.AccountService(1,~uint64_t(0));
    CHECK(p.Statistics().saturations>0);
    p.Enqueue(1);CHECK(p.PickNext()==1); // saturated arithmetic stays bounded/work conserving
    std::printf("EEVDF %s state=%zu weighted=%llu/%llu/%llu/%llu SMPspread=%llu weightedSMPerror=%llu\n",failures?"FAIL":"PASS",sizeof(p),
        (unsigned long long)service[1],(unsigned long long)service[2],(unsigned long long)service[3],(unsigned long long)service[4],
        (unsigned long long)equalSpread,(unsigned long long)weightedSmpError);
    return failures?1:0;
}
