#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
#include "../../kernel/src/sched_lock_owner.h"
#include "../../kernel/src/irq_accounting.h"
using namespace brook;
static std::atomic<unsigned> failures{0};
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"owner line %d: %s\n",__LINE__,#x);++failures;}}while(0)
int main(){
    ReadyLockOwnerObservation owner;ReadyLockOwnerSnapshot snap{};
    CHECK(!owner.Enabled() && !owner.Owner());owner.Acquired(1,7);CHECK(!owner.Owner());
    owner.Mode(true);owner.Acquired(1,UINT32_MAX);
    CHECK(owner.Owner()==owner.Pack(1,UINT32_MAX));CHECK(!owner.Pack(64,1));
    unsigned calls=0;uint32_t serving=UINT32_MAX;
    auto hint=[&](unsigned cpu,uint8_t* out){++calls;CHECK(cpu==1);*out=1;return true;};
    owner.Waiting(2);owner.Observe(2,[&]{return serving;},hint);
    owner.Read(3,&snap);CHECK(snap.cpu[2].kind[unsigned(OwnerWaitObservation::Preempted)]==1 && calls==1);
    owner.Observe(2,[]{return 0u;},hint);CHECK(calls==1); // mismatched ticket never reads unrelated CPU hint
    owner.Observe(1,[&]{return serving;},hint);CHECK(calls==1); // self
    owner.Observe(2,[&]{return serving;},[&](unsigned,uint8_t*){++serving;return true;}); // handoff during hint
    owner.Observe(2,[]{return UINT32_MAX;},[](unsigned,uint8_t*){return false;});
    owner.Observe(2,[]{return UINT32_MAX;},[](unsigned,uint8_t* out){*out=2;return true;});
    owner.Mode(false);owner.Releasing(1,UINT32_MAX);CHECK(!owner.Owner()); // disable must not discard cleanup
    owner.Read(3,&snap);CHECK(snap.cpu[1].published==1 && snap.cpu[1].cleared==1 && !snap.cpu[1].errors);
    owner.Mode(true);owner.Observe(2,[]{return 0u;},hint);CHECK(calls==1); // publication gap is explicit, not guessed holder
    owner.Acquired(3,0);CHECK(owner.CpuIndex(owner.Owner())==3 && owner.Ticket(owner.Owner())==0);
    owner.Releasing(3,0);owner.Read(4,&snap);
    for(unsigned c=0;c<4;++c){uint64_t sum=0;for(auto count:snap.cpu[c].kind)sum+=count;CHECK(sum==snap.cpu[c].observations);}
    CHECK(snap.cpu[2].kind[unsigned(OwnerWaitObservation::Changed)]==1);
    CHECK(snap.cpu[2].kind[unsigned(OwnerWaitObservation::MissingSteal)]==1);
    CHECK(snap.cpu[2].otherHints==1 && snap.cpu[2].kind[unsigned(OwnerWaitObservation::NotPreempted)]==1);
    // A successor must publish after the predecessor cleared and advanced. An
    // old release after advance would erase the new owner and fails this check.
    owner.Acquired(1,9);owner.Releasing(1,9);owner.Acquired(2,10);
    CHECK(owner.Owner()==owner.Pack(2,10));owner.Releasing(2,10);
    owner.Acquired(64,0);CHECK(!owner.Owner());owner.Observe(64,[]{return 1u;},hint);
    const auto old=snap.cpu[0].observations;owner.Read(64,nullptr);CHECK(snap.cpu[0].observations==old);

    // Actual atomic ticket admission, concurrent owners/readers, uint32 wrap,
    // normal and successful try claims. Publication never grants ownership.
    ReadyLockOwnerObservation concurrent;concurrent.Mode(true);
    volatile uint32_t next=UINT32_MAX-50,head=UINT32_MAX-50;
    std::atomic<unsigned> active{0};uint64_t protectedValue=0;
    std::vector<std::thread> threads;
    for(unsigned cpu=0;cpu<8;++cpu)threads.emplace_back([&,cpu]{
        for(unsigned i=0;i<3000;++i){
            uint32_t ticket=0;bool claimed=false;
            if(i%3==0)claimed=TicketTryClaim(&next,&head,[&](uint32_t t){while(__atomic_load_n(&head,__ATOMIC_ACQUIRE)!=t)std::this_thread::yield();},TicketNoHook{},[&](uint32_t t){ticket=t;});
            if(!claimed){ticket=__atomic_fetch_add(&next,1,__ATOMIC_RELAXED);bool waiting=false;
                while(__atomic_load_n(&head,__ATOMIC_ACQUIRE)!=ticket){
                    if(!waiting){concurrent.Waiting(cpu);waiting=true;}
                    concurrent.Observe(cpu,[&]{return __atomic_load_n(&head,__ATOMIC_ACQUIRE);},[](unsigned,uint8_t* out){*out=1;return true;});
                    std::this_thread::yield();
                }
            }
            concurrent.Acquired(cpu,ticket);CHECK(active.fetch_add(1)==0);
            CHECK(concurrent.Owner()==concurrent.Pack(cpu,ticket));++protectedValue;
            CHECK(active.fetch_sub(1)==1);concurrent.Releasing(cpu,ticket);
            __atomic_fetch_add(&head,1,__ATOMIC_RELEASE);
        }
    });
    for(auto& thread:threads)thread.join();concurrent.Mode(false);concurrent.Read(8,&snap);
    CHECK(protectedValue==24000 && !snap.owner && !snap.enabled && !snap.saturated && next==head);
    uint64_t published=0,cleared=0;
    for(unsigned c=0;c<8;++c){published+=snap.cpu[c].published;cleared+=snap.cpu[c].cleared;CHECK(!snap.cpu[c].errors && !snap.cpu[c].kind[unsigned(OwnerWaitObservation::Self)]);
        uint64_t sum=0;for(auto count:snap.cpu[c].kind)sum+=count;CHECK(sum==snap.cpu[c].observations);
    }
    CHECK(published==24000 && cleared==published);
    std::printf("READY_LOCK_OWNER %s\n",failures.load()?"FAIL":"PASS");return failures?1:0;
}
