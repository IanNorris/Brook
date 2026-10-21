#include <atomic>
#include <cstdio>
#include <functional>
#include <mutex>
#include <stdexcept>
#include "../../kernel/src/signal_restart.h"
namespace brook {
enum class ProcessState{Running,Ready,Blocked,Stopped,Terminated};
constexpr uint64_t PROCESS_MAGIC=0x12345;
struct Process{uint64_t magic=PROCESS_MAGIC;uint16_t pid=1;ProcessState state=ProcessState::Running;bool interruptibleWait=false,terminalPending=false;uint32_t pendingWakeup=0;uint64_t sigPending=0,sigMask=0;unsigned depth=0;};
struct SchedLock{};static SchedLock g_readyLock;static std::mutex ready;
static bool irq=true;static unsigned removes=0,voluntary=0,yields=0;
static ProcessState stateAtYield=ProcessState::Running;
static uint64_t timerPending=0;static std::function<void(Process*)> hook;
static struct {Process* currentProcess=nullptr;} g_perCpu[1];
static unsigned ThisCpu(){return 0;}
static uint64_t SchedLockAcquire(SchedLock&){ready.lock();return 0;}
static void SchedLockRelease(SchedLock&,uint64_t){ready.unlock();}
static uint64_t BlockHostSaveIrq(){bool old=irq;irq=false;return old?0x200:0;}
static void BlockHostRestoreIrq(uint64_t f){if(f&0x200)irq=true;}
static void BlockHostBeforeReadyLock(Process* p){if(hook)hook(p);}
[[noreturn]] static void KernelPanic(const char*,...){throw std::runtime_error("panic");}
static void ReadyQueueRemoveLocked(Process*){++removes;}
static void SchedTrace(Process*,unsigned){}
enum{STR_BLOCK_SKIP,STR_BLOCK};
static void YieldPolicy(void*,uint16_t){++voluntary;}
static struct{void(*VoluntaryYield)(void*,uint16_t)=YieldPolicy;} ops;
static auto* g_schedOps=&ops;static void* g_schedState=nullptr;
static void SchedulerYield(){stateAtYield=g_perCpu[0].currentProcess->state;++yields;g_perCpu[0].currentProcess->state=ProcessState::Running;}
static void SchedulerKernelRegionEnter(Process* p){++p->depth;}
static void SchedulerKernelRegionLeave(Process* p){--p->depth;}
[[maybe_unused]] static uint64_t PosixTimerPending(Process*){return timerPending;}
#define BROOK_SCHED_BLOCK_HOST_TEST
#include "../../kernel/src/scheduler_block.inc"
}
static unsigned failures=0;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"signal wait line %d: %s\n",__LINE__,#x);++failures;}}while(0)
int main(){using namespace brook;
    for(uint64_t n:{7,35,230,23,270,271,232,281,34,128,130})CHECK(!SignalSyscallMayRestart(n,0,0));
    for(uint64_t n:{0,1,2,16,17,18,19,20,61})CHECK(SignalSyscallMayRestart(n,0,0));
    for(uint64_t op:{0,128,9,137,393}){CHECK(SignalSyscallMayRestart(202,op,0));CHECK(!SignalSyscallMayRestart(202,op,0x1234));}
    CHECK(!SignalSyscallMayRestart(202,6,0));
    auto run=[&](uint64_t pending,uint64_t mask,bool terminal,bool interruptible,ProcessState state=ProcessState::Running){
        Process p;p.state=state;p.sigPending=pending;p.sigMask=mask;p.terminalPending=terminal;g_perCpu[0].currentProcess=&p;
        removes=voluntary=yields=0;irq=true;
        if(interruptible)SchedulerBlockInterruptible(&p);else SchedulerBlock(&p);
        CHECK(irq && !p.interruptibleWait && !p.depth);return yields;
    };
    CHECK(run(1,0,false,true)==0); // already pending, even if a futex cleared its wake latch
    CHECK(run(1,1,false,true)==1); // masked signal must not busy-loop an ordinary wait
    CHECK(run(1,0,false,false)==1); // uninterruptible mutex wait still needs an ownership grant
    CHECK(run(0,0,true,true)==0);
    // Ordinary signal delivery cannot bypass an already published SIGSTOP.
    CHECK(run(1,0,false,true,ProcessState::Stopped)==1);
    CHECK(stateAtYield==ProcessState::Stopped && removes==0 && voluntary==0);
    timerPending=1;CHECK(run(0,0,false,true)==0);timerPending=0;
    // Deterministic actual-body interleaving: last unlocked signal check found
    // none, signal publishes before block admission, unrelated code clears wake.
    hook=[](Process* p){__atomic_fetch_or(&p->sigPending,uint64_t(1),__ATOMIC_RELEASE);p->pendingWakeup=0;};
    CHECK(run(0,0,false,true)==0);hook={};
    // A retained ordinary wake still returns without becoming Blocked.
    hook=[](Process* p){p->pendingWakeup=1;};CHECK(run(0,0,false,true)==0);hook={};
    std::printf("SIGNAL_WAIT %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
