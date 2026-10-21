// Actual kernel notification and wait body. Mock IRQ masking, device memory,
// clock and scheduler admission; scheduler's own admission has separate tests.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include "../../kernel/src/gpu_completion.h"
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line%d %s\n",__LINE__,#x);std::abort();}}while(0)
namespace brook {
enum class ProcessState {Running,Stopped,Terminated};
struct Process {unsigned refs=1;uint64_t pendingWakeup=0,wakeupTick=777;bool terminalPending=false;ProcessState state=ProcessState::Running;uint64_t sigPending=0,sigMask=0;};
struct IrqSpinLock {};
static unsigned lockDepth,unblocks,blockCalls;
static uint64_t now=100000,tick=1;
static bool clockOk=true,retained=true,capacity=true;
static uint64_t contextFlags=0x200,timerPending;
static bool runtime=true;
static Process owner;
static uint8_t isr;
static uint16_t used,avail;
static std::function<void()> onUnlock,onBlock,onArm;
static void (*installedIrq)();
static uint64_t IrqSpinLockAcquire(IrqSpinLock*){CHECK(!lockDepth);++lockDepth;return 0;}
static void IrqSpinLockRelease(IrqSpinLock*,uint64_t){CHECK(lockDepth==1);--lockDepth;if(onUnlock){auto hook=std::move(onUnlock);onUnlock={};hook();}}
static void ProcessRef(Process* p){CHECK(p->refs);++p->refs;}
static void ProcessUnref(Process* p){CHECK(p->refs>1);--p->refs;}
static Process* ProcessCurrent(){return runtime?&owner:nullptr;}
static void SchedulerUnblock(Process* p){CHECK(!lockDepth&&p->refs>=2);++unblocks;}
static uint64_t CompletionHostFlags(){return contextFlags;}
static uint64_t PosixTimerPending(Process*){return timerPending;}
#define BROOK_GPU_COMPLETION_HOST_TEST
#include "../../kernel/src/gpu_completion_context.inc"
static bool KvmClockReadStableNs(uint64_t* out){*out=now;return clockOk;}
static uint64_t ApicTickCount(){return tick;}
static bool ModuleRetainAddresses(const uintptr_t*,uint32_t){return retained;}
static bool IoApicHandlerCapacity(uint8_t,void*){return capacity;}
static uint8_t IoApicRegisterHandler(uint8_t,uint8_t preferred,void* body){installedIrq=reinterpret_cast<void(*)()>(body);return preferred;}
static void CompletionHostAfterArm(){if(onArm)onArm();}
static void SchedulerBlockInterruptible(Process* p){
 CHECK(!lockDepth);++blockCalls;
 if(p->pendingWakeup){p->pendingWakeup=0;return;}
 if(!CompletionCanSleep(p))return;
 if(onBlock){auto hook=onBlock;hook();}
}
#include "../../kernel/src/gpu_completion.inc"
static void reset(){
 CHECK(owner.refs==1);g_completionLock={};g_completionStats={};g_completionIsr=nullptr;g_completionUsed=nullptr;g_completionAvailFlags=nullptr;
 g_completionWaiter=nullptr;g_completionGeneration=0;g_completionExpected=0;g_completionIrqNs=0;
 owner={};now=100000;tick=1;isr=0;used=0;avail=1;lockDepth=unblocks=blockCalls=0;
 clockOk=retained=capacity=runtime=true;contextFlags=0x200;timerPending=0;onUnlock={};onBlock={};onArm={};installedIrq=nullptr;
}
static void setup(){CHECK(GpuCompletionRegister(&owner,&isr,&used,&avail,11,false));CHECK(!GpuCompletionEnabled()&&avail==1);CHECK(GpuCompletionControl(true)&&avail==0);}
static void interrupt(uint8_t status){isr=status;CHECK(installedIrq);installedIrq();isr=0;}
}
using namespace brook;
int main(){
 reset();CHECK(!GpuCompletionControl(true));
 CHECK(!GpuCompletionRegister(&owner,&isr,&used,&avail,11,true));
 retained=false;CHECK(!GpuCompletionRegister(&owner,&isr,&used,&avail,11,false));retained=true;
 capacity=false;CHECK(!GpuCompletionRegister(&owner,&isr,&used,&avail,11,false));capacity=true;
 setup();CHECK(!GpuCompletionRegister(&owner,&isr,&used,&avail,11,false));
 // Shared-line zero and config-only status acknowledge without completion wake.
 interrupt(0);interrupt(2);CHECK(!unblocks&&GpuCompletionRead().irqCalls==2&&!GpuCompletionRead().queueIrqs);
 reset();setup();used=1;CHECK(GpuCompletionPause(0,1,10000000));CHECK(!blockCalls&&owner.refs==1&&GpuCompletionRead().readyBeforeBlock==1);
 // Completion becomes visible between arming and the mandatory recheck.
 reset();setup();onArm=[] {used=1;};CHECK(GpuCompletionPause(0,1,10000000));CHECK(!blockCalls&&owner.refs==1);
 // IRQ after recheck but before blocking leaves the admission wake latch set.
 reset();setup();onUnlock=[] {used=1;interrupt(1);};CHECK(GpuCompletionPause(0,1,10000000));CHECK(blockCalls==1&&unblocks==1&&owner.refs==1&&!g_completionWaiter);
 // Normal post-block completion, wrap of used index, and timestamp attribution.
 reset();setup();used=65535;onBlock=[] {used=0;interrupt(1);now+=250;};
 CHECK(GpuCompletionPause(65535,1,10000000));CHECK(unblocks==1&&owner.wakeupTick==777);
 CHECK(GpuCompletionRead().latencySamples==1&&GpuCompletionRead().latencyMaxNs==250);
 // Lost IRQ uses safety wake; repeated waits keep original publication deadline.
 reset();setup();onBlock=[] {tick=owner.wakeupTick;now=10000001;};
 CHECK(GpuCompletionPause(0,1,10000000));CHECK(GpuCompletionRead().safetyExpiries==1&&!unblocks);
 CHECK(!GpuCompletionPause(0,1,10000000)&&owner.refs==1);
 // Disable during admission suppresses notifications and wakes exact old waiter.
 reset();setup();onUnlock=[] {CHECK(GpuCompletionControl(false));};
 CHECK(GpuCompletionPause(0,1,10000000));CHECK(avail==1&&!g_completionWaiter&&owner.refs==1&&unblocks==1);
 // Old cleanup cannot clear a new registration generation after off/on.
 reset();setup();onBlock=[] {
  CHECK(GpuCompletionControl(false));CHECK(GpuCompletionControl(true));owner.pendingWakeup=0;
  onBlock=[] {used=1;interrupt(1);};CHECK(GpuCompletionPause(0,1,10000000));
 };
 CHECK(GpuCompletionPause(0,1,10000000));CHECK(owner.refs==1&&!g_completionWaiter);
 reset();setup();runtime=false;CHECK(!GpuCompletionPause(0,1,10000000));runtime=true;
 contextFlags=0;CHECK(!GpuCompletionPause(0,1,10000000));contextFlags=0x200;
 owner.terminalPending=true;CHECK(!GpuCompletionPause(0,1,10000000));owner.terminalPending=false;
 owner.state=ProcessState::Stopped;CHECK(!GpuCompletionPause(0,1,10000000));owner.state=ProcessState::Running;
 owner.sigPending=2;CHECK(!GpuCompletionPause(0,1,10000000));owner.sigPending=0;
 timerPending=4;CHECK(!GpuCompletionPause(0,1,10000000));timerPending=0;
 clockOk=false;CHECK(!GpuCompletionPause(0,1,10000000));clockOk=true;
 now=10;CHECK(!GpuCompletionPause(0,100,10000000));now=100;
 CHECK(!GpuCompletionPause(0,1,10000000)); // initial short spin
 CHECK(owner.refs==1&&!blockCalls);
 reset();setup();owner.sigPending=2;owner.sigMask=2;onBlock=[] {used=1;interrupt(1);};
 CHECK(GpuCompletionPause(0,1,10000000)&&owner.refs==1);
 reset();setup();onUnlock=[] {owner.terminalPending=true;};
 CHECK(GpuCompletionPause(0,1,10000000)&&!blockCalls&&owner.refs==1&&!g_completionWaiter);
 reset();setup();onUnlock=[] {owner.sigPending=2;};
 CHECK(GpuCompletionPause(0,1,10000000)&&!blockCalls&&owner.refs==1&&!g_completionWaiter);
 reset();setup();g_completionGeneration=~uint64_t(0);CHECK(!GpuCompletionPause(0,1,10000000));
 reset();std::puts("GPU_COMPLETION_EVENT_ACTUAL_BODY_PASS");
}
