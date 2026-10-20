#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
#include "../../kernel/src/wake_scan_handoff.h"
namespace {std::atomic<int> failures{0};}
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"wake handoff line %d: %s\n",__LINE__,#x);++failures;}}while(0)
int main(){
    brook::WakeScanHandoff m;CHECK(!m.Shared());
    CHECK(!m.Begin(1,100000000).sequence);CHECK(m.Control(1,false));
    CHECK(!m.Begin(1,100000000).sequence); // no valid completion anchor yet
    auto bsp=m.Begin(0,1000000);CHECK(bsp.sequence);
    CHECK(!m.Begin(0,100000000).sequence && !m.Begin(1,100000000).sequence);
    CHECK(!m.Control(0,true));CHECK(m.Shared()); // cannot revoke active owner
    CHECK(m.Complete(bsp,2000000,3));CHECK(!m.Complete(bsp,2000001,999));
    CHECK(!m.Begin(1,3999999).sequence);auto ap=m.Begin(1,4000000);CHECK(ap.sequence);
    CHECK(!m.Complete(bsp,4000001,999));CHECK(!m.Begin(2,1000000000).sequence);
    // Gathering and publication/reference cleanup remain in this one claim,
    // regardless of how long the owner is delayed.
    CHECK(m.Complete(ap,5000000,2));auto s=m.Snapshot();
    CHECK(s.consistent && s.bspScans==1 && s.apScans==1 && s.candidates==5);
    CHECK(s.lastCpu==1 && s.maxGapNs==3000000 && s.maxDurationNs==1000000);
    CHECK(!m.Begin(2,0).sequence && !m.Begin(2,4999999).sequence);
    CHECK(m.Control(0,false));CHECK(!m.Shared() && m.Snapshot().candidates==5);
    CHECK(!m.Begin(2,100000000).sequence);bsp=m.Begin(0,0);CHECK(bsp.sequence);
    CHECK(m.Complete(bsp,0,0));CHECK(m.Snapshot().invalidClock==1);
    CHECK(m.Control(1,false));CHECK(!m.Begin(1,100000000).sequence);
    bsp=m.Begin(0,100);CHECK(m.Complete(bsp,200,0));
    bsp=m.Begin(0,300);CHECK(m.Complete(bsp,199,0));CHECK(m.Snapshot().invalidClock==2);
    CHECK(m.Control(-1,true));s=m.Snapshot();CHECK(s.shared && !s.bspScans && !s.apScans && !s.lastCompletionNs && !s.invalidClock);
    CHECK(!m.Control(-2,true) && !m.Control(2,true));
    bsp=m.Begin(0,1);CHECK(m.Complete(bsp,2,~uint64_t(0)));
    bsp=m.Begin(0,3);CHECK(m.Complete(bsp,4,1));CHECK(m.Snapshot().saturated && m.Snapshot().candidates==~uint64_t(0));

    // Real concurrent claimants contend on the same production atomic model.
    // A separate active counter detects overlap across the entire protected
    // work region, including a delayed cleanup tail before Complete.
    brook::WakeScanHandoff concurrent(0);CHECK(concurrent.Control(1,true));
    auto initial=concurrent.Begin(0,1);CHECK(concurrent.Complete(initial,2,0));
    std::atomic<uint64_t> clock{3},completed{0};std::atomic<unsigned> active{0},ready{0};std::atomic<bool> go{false};
    std::atomic<unsigned> turn{0};std::atomic<uint64_t> wins[8]{};
    std::vector<std::thread> threads;
    for(unsigned cpu=0;cpu<8;++cpu)threads.emplace_back([&,cpu]{
        ++ready;while(!go.load())std::this_thread::yield();
        auto attempt=[&](unsigned n){
            auto token=concurrent.Begin(cpu,clock.fetch_add(1));if(!token.sequence)return false;
            CHECK(active.fetch_add(1)==0);
            if(n%17==0)std::this_thread::yield();
            CHECK(!concurrent.Begin(cpu,clock.fetch_add(1)).sequence);
            CHECK(!concurrent.Control(0,true));
            CHECK(active.fetch_sub(1)==1);
            CHECK(concurrent.Complete(token,clock.fetch_add(1),1));++completed;++wins[cpu];return true;
        };
        // Guarantee each real worker exercises completion, then release all
        // workers into contention. A bounded try-claim loop does not promise
        // an AP win: on one host CPU, BSP can own while all AP attempts expire.
        // This controlled phase is coverage, not a starvation/fairness proof.
        while(turn.load()!=cpu)std::this_thread::yield();
        CHECK(attempt(0));++turn;
        while(turn.load()!=8)std::this_thread::yield();
        for(unsigned n=0;n<10000;++n)attempt(n);
    });
    while(ready.load()!=8)std::this_thread::yield();go=true;
    for(auto& t:threads)t.join();s=concurrent.Snapshot();
    if(!(s.consistent && !active && completed>=8 && s.apScans>=7))
        std::fprintf(stderr,"handoff snapshot: consistent=%u active=%u completed=%llu bsp=%llu ap=%llu invalid=%llu\n",
            unsigned(s.consistent),active.load(),(unsigned long long)completed.load(),
            (unsigned long long)s.bspScans,(unsigned long long)s.apScans,(unsigned long long)s.invalidClock);
    CHECK(s.consistent && !active && completed>=8 && s.apScans>=7);
    for(auto& count:wins)CHECK(count.load()>0);
    CHECK(s.bspScans+s.apScans==completed+1 && s.candidates==completed && !s.invalidClock && !s.saturated);
    std::printf("WAKE_SCAN_HANDOFF %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
