// Actual subscription and pipe storage code; scheduler/reaper and heap boundaries
// are instrumented. Tests keep endpoint counts separate from poll storage holds.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>
#include <memory>
#include "../../kernel/src/pipe.h"
namespace brook {
struct Process { std::atomic<int> refs{1}, wakes{0}; };
static std::atomic<int> allocations{0};
static bool failAllocation;
extern "C" void* kmalloc(uint64_t size) {
    if (failAllocation) return nullptr;
    void* p=std::malloc(size);if(p)++allocations;return p;
}
extern "C" void kfree(void* p) { if(p){--allocations;std::free(p);} }
static void ProcessRef(Process* p) { ++p->refs; }
static void ProcessUnref(Process* p) { --p->refs; }
static std::function<void(Process*)> onWake;
static void WakeProcess(Process* p) { ++p->wakes;if(onWake)onWake(p); }
#include "../../kernel/src/pipe_poll.inc"
}
using namespace brook;
static unsigned failures;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line%d %s\n",__LINE__,#x);++failures;}}while(0)
int main() {
    for(bool reverse : {false,true}) {
        PipeBuffer pipe;Process tasks[3], ordinary;
        pipe.readers=pipe.writers=1;pipe.readerWaiter=&ordinary;
        PipePollGroup groups[3];
        for(unsigned j=0;j<3;++j) {unsigned i=reverse?2-j:j;CHECK(groups[i].add(&pipe,&tasks[i],1));}
        CHECK(pipe.pollHolds==3 && pipe.readers==1 && pipe.writers==1);
        PipePollNotify(&pipe,1);
        for(auto& p:tasks)CHECK(p.wakes==1 && p.refs==2);
        // Every transition notifies again: a wake may precede a later block.
        PipePollNotify(&pipe,1);
        for(auto& p:tasks)CHECK(p.wakes==2);
        CHECK(pipe.readerWaiter==&ordinary);
    }
    CHECK(allocations==0);
    {
        PipeBuffer pipe;Process a,b;
        auto g=std::make_unique<PipePollGroup>();
        CHECK(g->add(&pipe,&a,1));CHECK(g->add(&pipe,&a,4));
        CHECK(pipe.pollHolds==1 && a.refs==2); // duplicated FDs merge interest
        PipePollGroup other;CHECK(other.add(&pipe,&b,4));
        PipePollNotify(&pipe,1);CHECK(a.wakes==1 && b.wakes==0);
        g.reset();CHECK(a.refs==1 && pipe.pollHolds==1);
        PipePollNotify(&pipe,4);CHECK(b.wakes==1 && a.wakes==1);
    }
    {
        PipeBuffer pipe;Process p;
        auto group=std::make_unique<PipePollGroup>();CHECK(group->add(&pipe,&p,1));
        onWake=[&](Process* q) {
            CHECK(q->refs==3); // registration + notifier + initial owner
            CHECK(SpinLockTryAcquire(&pipe.lock));SpinLockRelease(&pipe.lock);
            group.reset();CHECK(q->refs==2); // node freed during unlocked wake
        };
        PipePollNotify(&pipe,1);onWake={};CHECK(p.refs==1 && !pipe.pollWaiters);
    }
    {
        auto* pipe=PipeBufferCreate(16);Process p;
        auto group=std::make_unique<PipePollGroup>();CHECK(group->add(pipe,&p,1));
        PipeBufferDestroy(pipe);
        onWake=[&](Process* q) {group.reset();CHECK(q->refs==2 && allocations==2);};
        PipePollNotify(pipe,1);onWake={};
        CHECK(p.refs==1 && allocations==0); // notifier releases final storage hold
    }
    {
        PipeBuffer oldBackend,newBackend;Process p;
        auto group=std::make_unique<PipePollGroup>();
        PipeBuffer* fd=&oldBackend;CHECK(group->add(fd,&p,1));
        fd=&newBackend; // close/reuse changes numeric slot, never unregister target
        CHECK(group->add(fd,&p,1));group.reset();
        CHECK(!oldBackend.pollWaiters && !newBackend.pollWaiters && p.refs==1);
    }
    {
        auto* pipe=PipeBufferCreate(16);Process p;
        auto group=std::make_unique<PipePollGroup>();CHECK(group->add(pipe,&p,1));
        CHECK(allocations==3);PipeBufferDestroy(pipe);
        CHECK(pipe->destroyPending && pipe->writers==0 && pipe->readers==0);
        PipePollNotify(pipe,1);CHECK(p.wakes==1);group.reset();CHECK(allocations==0 && p.refs==1);
    }
    {
        PipeBuffer pipe;Process a,b;PipePollGroup group;
        CHECK(group.add(&pipe,&a,1));failAllocation=true;
        CHECK(group.add(&pipe,&a,4)); // deduplicated interest needs no allocation
        PipeBuffer second;CHECK(!group.add(&second,&b,1));failAllocation=false;
        CHECK(!second.pollHolds && !second.pollWaiters && b.refs==1);
    }
    {
        PipeBuffer pipe;Process p;pipe.pollSequence=~0ULL;
        PipePollGroup group;CHECK(!group.add(&pipe,&p,1));CHECK(p.refs==1 && !pipe.pollHolds);
    }
    // Real competing pthread registration/removal and overlapping notifiers.
    {
        PipeBuffer pipe;Process tasks[4];std::atomic<bool> stop{false};
        std::thread notifier([&]{while(!stop.load())PipePollNotify(&pipe,1|4);});
        std::vector<std::thread> workers;
        for(auto& task:tasks)workers.emplace_back([&pipe,&task]{
            for(unsigned i=0;i<3000;++i){PipePollGroup group;group.add(&pipe,&task,1|4);PipePollNotify(&pipe,1);}
        });
        for(auto& t:workers)t.join();
        stop=true;notifier.join();
        CHECK(!pipe.pollHolds && !pipe.pollWaiters);
        for(auto& p:tasks)CHECK(p.refs==1 && p.wakes>0);
    }
    CHECK(allocations==0);
    std::printf("PIPE_POLL_ACTUAL_BODY %s failures=%u\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
