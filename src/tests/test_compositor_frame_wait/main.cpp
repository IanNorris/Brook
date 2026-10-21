#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>
#include "../../kernel/src/compositor_wake_sequence.h"
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line%d %s\n",__LINE__,#x);std::abort();}}while(0)
namespace brook {
struct Process {uint64_t wakeupTick=0;bool pending=false;};
static Process owner;
static CompositorWakeSequence g_compositorWakeSequence;
static constexpr unsigned COMPOSITE_INTERVAL=16;
static unsigned admissionCalls,actualBlocks;
static std::function<void()> beforeAdmission,onEnd,onBlocked;
static Process* ProcessCurrent(){return &owner;}
static uint64_t ApicTickCount(){return 100;}
static void FrameTimingEnd(uint64_t tick){CHECK(tick==116);if(onEnd)onEnd();}
static void SchedulerBlock(Process* p){
 ++admissionCalls;if(beforeAdmission)beforeAdmission();
 if(p->pending){p->pending=false;return;}
 ++actualBlocks;if(onBlocked)onBlocked();
}
#include "../../kernel/src/compositor_frame_wait.inc"
static void wake(){g_compositorWakeSequence.Publish();owner.pending=true;}
static void reset(){owner={};g_compositorWakeSequence={};admissionCalls=actualBlocks=0;beforeAdmission={};onEnd={};onBlocked={};}
}
using namespace brook;
int main(){
 reset();CompositorWaitForWork(g_compositorWakeSequence.Snapshot());CHECK(admissionCalls==1&&actualBlocks==1);
 // Regression: work arrives during a frame, but a GPU wait consumes its latch.
 reset();auto frame=g_compositorWakeSequence.Snapshot();wake();owner.pending=false;
 CompositorWaitForWork(frame);CHECK(!admissionCalls&&!actualBlocks&&owner.wakeupTick==0);
 // A new frame observes that generation; absent newer work it sleeps normally.
 frame=g_compositorWakeSequence.Snapshot();CompositorWaitForWork(frame);CHECK(actualBlocks==1);
 reset();frame=g_compositorWakeSequence.Snapshot();onEnd=[] {wake();owner.pending=false;};
 CompositorWaitForWork(frame);CHECK(!admissionCalls&&owner.wakeupTick==0);
 reset();frame=g_compositorWakeSequence.Snapshot();beforeAdmission=[] {wake();};
 CompositorWaitForWork(frame);CHECK(admissionCalls==1&&!actualBlocks);
 reset();frame=g_compositorWakeSequence.Snapshot();onBlocked=[] {wake();};
 CompositorWaitForWork(frame);CHECK(actualBlocks==1&&g_compositorWakeSequence.Changed(frame));
 reset();g_compositorWakeSequence.sequence=~uint64_t(0)-1;g_compositorWakeSequence.Publish();
 frame=g_compositorWakeSequence.Snapshot();g_compositorWakeSequence.Publish();
 CHECK(frame==~uint64_t(0)&&g_compositorWakeSequence.Changed(frame));CompositorWaitForWork(frame);CHECK(!admissionCalls);
 reset();std::vector<std::thread> workers;
 for(unsigned i=0;i<4;++i)workers.emplace_back([] {for(unsigned j=0;j<10000;++j)g_compositorWakeSequence.Publish();});
 for(auto& thread:workers)thread.join();CHECK(g_compositorWakeSequence.Snapshot()==40000);
 std::puts("COMPOSITOR_FRAME_WAIT_ACTUAL_BODY_PASS");
}
