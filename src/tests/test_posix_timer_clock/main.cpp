#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include "../../kernel/src/posix_timer.cpp"

namespace {
uint64_t monotonicNs = 9750000000ULL;
uint64_t realtimeNs = 1700000000250000000ULL;
bool pvclock = true;
uint64_t signalHandler = 0x1234;
brook::Process* timerTarget = nullptr;
unsigned failures = 0;
#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "POSIX CLOCK FAIL line %d: %s\n", __LINE__, #x); \
    ++failures; } } while (0)
}

namespace brook {
volatile uint64_t g_lapicTickCount = 0;
uint64_t ApicTickCount() { return g_lapicTickCount; }
bool KvmClockAvailable() { return pvclock; }
uint64_t KvmClockReadNs() { return monotonicNs; }
uint64_t RtcNowNs() { return realtimeNs; }
uint64_t RtcNow() { return realtimeNs / 1000000000ULL; }
KernelSigaction ReadSignalAction(uint16_t,uint32_t) {
    KernelSigaction action{}; action.handler=signalHandler; return action;
}
bool SchedulerTimerTargetIncarnation(Process* owner,int32_t tid,uint64_t* incarnation) {
    if(!timerTarget || timerTarget->tgid!=owner->tgid || timerTarget->pid!=tid ||
       timerTarget->state==ProcessState::Terminated)return false;
    *incarnation=timerTarget->incarnation;return true;
}

// Execute real block/unblock bodies with real timer code and Process layout.
// Only IRQ, context switch, scheduler queues and parent side effects are shims.
struct SchedLock{};
static SchedLock g_readyLock,g_allProcLock;
static std::mutex readyMutex,registryMutex;
static uint64_t SchedLockAcquire(SchedLock& lock) {
    (&lock==&g_readyLock?readyMutex:registryMutex).lock();return 0;
}
static void SchedLockRelease(SchedLock& lock,uint64_t) {
    (&lock==&g_readyLock?readyMutex:registryMutex).unlock();
}
static bool irq=true;
static unsigned yields=0,inserts=0,removes=0,voluntary=0;
static std::function<void()> beforeAdmission,afterBlock,atYield;
static struct {Process* currentProcess=nullptr;} g_perCpu[1];
static unsigned ThisCpu(){return 0;}
static uint64_t BlockHostSaveIrq(){bool previous=irq;irq=false;return previous?0x200:0;}
static void BlockHostRestoreIrq(uint64_t flags){if(flags&0x200)irq=true;}
static void BlockHostBeforeReadyLock(Process*){if(beforeAdmission)beforeAdmission();}
[[noreturn]] static void KernelPanic(const char*,...){std::abort();}
enum {STR_BLOCK_SKIP,STR_BLOCK,STR_UNBLOCK_DEFER_RR,STR_UNBLOCK_DEFER_CPU,STR_READY_UNBLOCK};
static void SchedTrace(Process*,unsigned){}
static void ReadyQueueRemoveLocked(Process*){++removes;}
static void ReadyQueueInsertLocked(Process*){++inserts;}
static void NotifyParentGroupLocked(Process*){}
static void KickIdleCpu(){}
static void PolicyVoluntaryYield(void*,uint16_t){++voluntary;if(afterBlock)afterBlock();}
static SchedOps policyOps{};
static const SchedOps* g_schedOps=&policyOps;
static void* g_schedState=nullptr;
static struct {bool Watching(uint16_t,uint64_t){return false;}void Deadline(uint16_t,uint64_t,uint64_t,uint64_t){}} g_readyLatency;
void SchedulerKernelRegionEnter(Process* p){++p->kernelRegionDepth;}
void SchedulerKernelRegionLeave(Process* p){--p->kernelRegionDepth;}
extern "C" void SchedulerYield(){
    ++yields;if(atYield)atYield();g_perCpu[0].currentProcess->state=ProcessState::Running;
}
#define BROOK_SCHED_BLOCK_HOST_TEST
#include "../../kernel/src/scheduler_block.inc"
#include "../../kernel/src/scheduler_unblock.inc"
}

namespace {
void init(brook::Process& p,uint16_t pid=7) {
    using namespace brook;
    p.magic=PROCESS_MAGIC;p.pid=p.tgid=pid;p.incarnation=100+pid;p.state=ProcessState::Running;
    p.sigPending=p.sigMask=0;p.pendingWakeup=0;p.interruptibleWait=p.terminalPending=p.resumePending=false;
    p.kernelRegionDepth=0;p.isThread=false;p.leaderIncarnation=0;p.tgidExiting=false;
    p.runningOnCpu=0;p.asLiveThreads=1;g_perCpu[0].currentProcess=&p;
    policyOps.VoluntaryYield=PolicyVoluntaryYield;
    yields=inserts=removes=voluntary=0;irq=true;beforeAdmission=afterBlock=atYield={};
    signalHandler=0x1234;pvclock=true;timerTarget=nullptr;
}
int32_t arm(brook::Process& p,int clock=1,int notify=0) {
    using namespace brook;
    PosixSigevent event{};event.notify=notify;event.signo=10;event.value=0x123456789ULL;
    if(notify==4)event.targetTid=timerTarget?timerTarget->pid:0;
    int32_t id=0;CHECK(PosixTimerCreate(&p,clock,&event,&id)==0);
    CHECK(PosixTimerSet(&p,id,0,{{0,0},{0,5000000}},nullptr)==0);return id;
}
void expire(brook::Process& p) {
    monotonicNs+=5000000;realtimeNs+=5000000;
    brook::Process* processes[]={&p};brook::PosixTimersTickLocked(processes,1);
}
void timerAdmission() {
    using namespace brook;
    constexpr uint64_t bit=1ULL<<9;
    for(unsigned phase=0;phase<6;++phase){
        Process p{};init(p);const auto id=arm(p);
        const bool admitted=phase==3 || phase==4;
        if(phase==0)expire(p);
        if(phase==1 || phase==2)beforeAdmission=[&]{expire(p);if(phase==2)p.pendingWakeup=0;};
        if(phase==3)afterBlock=[&]{expire(p);}; // ready lock held; expiry cannot reacquire it
        if(phase==5){ // querying an expired timer can publish pending without a tick/latch
            monotonicNs+=5000000;PosixItimerspec value{};
            CHECK(PosixTimerGet(&p,id,&value)==0 && !p.pendingWakeup);
        }
        if(admitted)atYield=[&]{
            if(phase==4)expire(p);
            CHECK(p.state==ProcessState::Blocked && p.pendingWakeup && PosixTimerPending(&p)==bit);
            // The context-switch/wake-scan boundary is shimmed. The actual
            // unblock body must enqueue only after stack ownership is released.
            p.runningOnCpu=-1;SchedulerUnblock(&p);CHECK(p.state==ProcessState::Ready);
        };
        SchedulerBlockInterruptible(&p);
        CHECK(yields==(admitted?1u:0u) && inserts==(admitted?1u:0u));
        CHECK(removes==(admitted?1u:0u) && voluntary==(admitted?1u:0u));
        CHECK(irq && !p.interruptibleWait && !p.kernelRegionDepth);
        CHECK(PosixTimerPending(&p)==bit);
        SignalInfo info{};CHECK(PosixTimerDequeue(&p,bit,&info)==10);
        uint64_t value=0;__builtin_memcpy(&value,info._data+8,8);
        CHECK(info.si_code==-2 && value==0x123456789ULL && !PosixTimerPending(&p));
        CHECK(PosixTimerDequeue(&p,bit,&info)==0 && PosixTimerDelete(&p,id)==0);
    }
    // Masked signals remain synchronously consumable even when ignored.
    for(uint64_t disposition:{0x1234ULL,1ULL}){
        Process p{};init(p);p.sigMask=bit;signalHandler=disposition;const auto id=arm(p);
        beforeAdmission=[&]{expire(p);};SchedulerBlockInterruptible(&p);
        CHECK(!yields && !p.pendingWakeup && PosixTimerPending(&p)==bit);
        SignalInfo info{};CHECK(PosixTimerDequeue(&p,bit,&info)==10 && !PosixTimerPending(&p));
        CHECK(PosixTimerDelete(&p,id)==0);
    }
    Process p{};init(p);signalHandler=1;auto id=arm(p);expire(p);
    CHECK(!p.pendingWakeup && !PosixTimerPending(&p));
    SchedulerBlockInterruptible(&p);CHECK(yields==1);CHECK(PosixTimerDelete(&p,id)==0);
    init(p);id=arm(p,1,1);expire(p); // SIGEV_NONE cannot interrupt a waiter
    CHECK(!p.pendingWakeup && !PosixTimerPending(&p));CHECK(PosixTimerDelete(&p,id)==0);
}

void timerDomainsAndOwnership() {
    using namespace brook;
    Process p{};init(p);const auto id=arm(p,0);
    Timer* timer=Lookup(&p,id);CHECK(timer && timer->basis==1); // relative REALTIME uses elapsed time
    monotonicNs+=4000000;realtimeNs+=1000000000;
    PosixItimerspec remaining{};CHECK(PosixTimerGet(&p,id,&remaining)==0);
    CHECK(remaining.value.sec==0 && remaining.value.nsec==1000000);
    const auto absolute=realtimeNs+7000000;
    CHECK(PosixTimerSet(&p,id,1,{{0,0},FromNs(absolute)},nullptr)==0 && timer->basis==0);
    monotonicNs+=1000000000;CHECK(PosixTimerGet(&p,id,&remaining)==0 && remaining.value.nsec==7000000);
    const auto deadline=timer->deadline;
    for(PosixItimerspec invalid: {PosixItimerspec{{0,0},{-1,0}},{{0,0},{0,1000000000}},{{0,0},{INT64_MAX,0}},{{-1,0},{0,1}}}) {
        CHECK(PosixTimerSet(&p,id,0,invalid,nullptr)==-22 && timer->deadline==deadline);
    }
    const auto savedMono=monotonicNs;monotonicNs=~uint64_t(0)-4;
    CHECK(PosixTimerSet(&p,id,0,{{0,0},{0,5}},nullptr)==-22 && timer->deadline==deadline);
    monotonicNs=savedMono;CHECK(PosixTimerDelete(&p,id)==0);

    init(p);Process peer{};init(peer,8);peer.tgid=p.tgid;peer.isThread=true;peer.leaderIncarnation=p.incarnation;
    timerTarget=&peer;const auto target=arm(p,1,4);monotonicNs+=5000000;
    Process* processes[]={&p,&peer};PosixTimersTickLocked(processes,2);
    CHECK(!p.pendingWakeup && peer.pendingWakeup && !PosixTimerPending(&p) && PosixTimerPending(&peer)==(1ULL<<9));
    // Same PID with a new incarnation cannot consume an old targeted event.
    ++peer.incarnation;CHECK(!PosixTimerPending(&peer));
    SignalInfo info{};CHECK(PosixTimerDequeue(&peer,~uint64_t(0),&info)==0);
    CHECK(PosixTimerDelete(&p,target)==0);
    CHECK(!usedCount && !pendingCount);
}
void timerLifecycle() {
    using namespace brook;
    constexpr uint64_t bit=1ULL<<9;
    Process p{};init(p);p.sigMask=bit;auto id=arm(p);
    CHECK(PosixTimerSet(&p,id,0,{{0,2000000},{0,5000000}},nullptr)==0);
    monotonicNs+=15000000;Process* processes[]={&p};PosixTimersTickLocked(processes,1);
    CHECK(p.pendingWakeup && pendingCount==1);p.pendingWakeup=0;
    monotonicNs+=4000000;PosixTimersTickLocked(processes,1);
    CHECK(!p.pendingWakeup && pendingCount==1); // pending notification is not requeued
    SignalInfo info{};CHECK(PosixTimerDequeue(&p,bit,&info)==10);
    int overrun=0;__builtin_memcpy(&overrun,info._data+4,4);
    CHECK(overrun==7 && PosixTimerOverrun(&p,id)==7 && !pendingCount);
    monotonicNs+=2000000;PosixTimersTickLocked(processes,1);
    CHECK(p.pendingWakeup && pendingCount==1);
    p.asLiveThreads=0;PosixTimersCleanup(&p,false);
    CHECK(!usedCount && !pendingCount && !PosixTimerPending(&p));
    PosixItimerspec removed{};CHECK(PosixTimerGet(&p,id,&removed)==-22);
    init(p);id=arm(p);expire(p);CHECK(pendingCount==1);
    CHECK(PosixTimerDelete(&p,id)==0 && !pendingCount);
    const auto fresh=arm(p);CHECK(fresh>id && PosixTimerDelete(&p,fresh)==0);
    // A dead owner cannot receive or leave a deliverable notification behind.
    id=arm(p);p.state=ProcessState::Terminated;expire(p);
    CHECK(!pendingCount && !PosixTimerPending(&p));PosixTimersCleanup(&p,true);
    CHECK(!usedCount && !pendingCount);
    // Saturate overrun accounting and disarm rather than wrap a periodic
    // deadline when no later representable nanosecond exists.
    init(p);id=arm(p);CHECK(PosixTimerSet(&p,id,1,{{0,1},{0,1}},nullptr)==0);
    const auto saved=monotonicNs;monotonicNs=~uint64_t(0);
    PosixTimersTickLocked(processes,1);CHECK(pendingCount==1 && !Lookup(&p,id)->armed);
    CHECK(PosixTimerDequeue(&p,bit,&info)==10 && PosixTimerOverrun(&p,id)==0x7fffffff);
    monotonicNs=saved;CHECK(PosixTimerDelete(&p,id)==0 && !pendingCount && !usedCount);
}
}

int main() {
    using namespace brook;
    // Deliberately unrelated fractional epochs. Exercise the production POSIX
    // clock selector and expiry code, not a second implementation of either.
    CHECK(Now(0) == realtimeNs);
    CHECK(Now(1) == monotonicNs);
    Timer timer{};
    timer.armed = true;
    timer.notify = 1; // SIGEV_NONE: observe expiry without signal scaffolding.
    timer.deadline = realtimeNs + 100000000ULL;
    Advance(timer, Now(0));
    CHECK(timer.armed);
    // Cross the pvclock second boundary while realtime stays in its second.
    monotonicNs += 250000000ULL;
    realtimeNs += 250000000ULL;
    CHECK(Now(0) == realtimeNs);
    Advance(timer, Now(0));
    CHECK(!timer.armed);
    // A realtime boundary must not borrow the monotonic fractional second.
    realtimeNs = 1700000000999999999ULL;
    CHECK(Now(0) == realtimeNs);
    ++realtimeNs;
    CHECK(Now(0) == realtimeNs);
    // Non-KVM realtime must retain the RTC driver's own elapsed source.
    pvclock = false;
    g_lapicTickCount = 123;
    CHECK(Now(0) == realtimeNs);
    CHECK(Now(1) == 123000000ULL);
    timerAdmission();
    timerDomainsAndOwnership();
    timerLifecycle();
    std::printf("POSIX timer clock: %u failures\n", failures);
    return failures ? 1 : 0;
}
