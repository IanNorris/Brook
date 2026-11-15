#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include "../../kernel/src/posix_timer.cpp"

namespace {
uint64_t monotonicNs = 9750000000ULL;
uint64_t realtimeNs = 1700000000250000000ULL;
bool pvclock = true;
std::atomic<uint64_t> signalHandler{0x1234};
std::function<void()> actionReadHook;
brook::Process* timerTarget = nullptr;
unsigned failures = 0;
#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "POSIX CLOCK FAIL line %d: %s\n", __LINE__, #x); \
    ++failures; } } while (0)
}

namespace brook {
volatile uint64_t g_lapicTickCount = 0;
uint64_t ApicTickCount() { return g_lapicTickCount; }
bool TscClockAvailable() { return false; }
uint64_t TscClockReadNs() { return 0; }
bool KvmClockAvailable() { return pvclock; }
uint64_t KvmClockReadNs() { return monotonicNs; }
uint64_t RtcNowNs() { return realtimeNs; }
uint64_t RtcNow() { return realtimeNs / 1000000000ULL; }
KernelSigaction ReadSignalAction(uint16_t,uint32_t) {
    if(actionReadHook)actionReadHook();
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
uint64_t ProcessPendingSignals(Process* p){return p->sigPending;}
#include "../../kernel/src/scheduler_block.inc"
#include "../../kernel/src/scheduler_unblock.inc"
}

namespace {
void init(brook::Process& p,uint16_t pid=7) {
    using namespace brook;
    p.magic=PROCESS_MAGIC;p.pid=p.tgid=pid;p.incarnation=100+pid;p.state=ProcessState::Running;
    p.sigPending=p.sigMask=0;p.pendingWakeup=0;p.interruptibleWait=p.terminalPending=p.resumePending=false;
    p.wakeAfterSwitch = false;
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
    for(PosixItimerspec invalid: {PosixItimerspec{{0,0},{-1,0}},{{0,0},{0,1000000000}},{{0,0},{INT64_MAX,1000000000}},{{-1,0},{0,1}}}) {
        CHECK(PosixTimerSet(&p,id,0,invalid,nullptr)==-22 && timer->deadline==deadline);
    }
    const auto savedMono=monotonicNs;monotonicNs=uint64_t(INT64_MAX)-4;
    CHECK(PosixTimerSet(&p,id,0,{{0,0},{0,5}},nullptr)==0 && timer->deadline==uint64_t(INT64_MAX));
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
    init(p);id=arm(p);
    CHECK(PosixTimerSet(&p,id,0,{{INT64_MAX,0},{0,1}},nullptr)==0);
    ++monotonicNs;PosixTimersTickLocked(processes,1);
    auto* large=Lookup(&p,id);CHECK(large && large->interval==uint64_t(INT64_MAX));
    CHECK(large && large->deadline==uint64_t(INT64_MAX) && large->armed && pendingCount==1);
    PosixTimersTickLocked(processes,1);CHECK(pendingCount==1);
    CHECK(PosixTimerDequeue(&p,bit,&info)==10);
    const auto beforeHorizon=monotonicNs;monotonicNs=uint64_t(INT64_MAX);
    PosixTimersTickLocked(processes,1);CHECK(pendingCount==1 && !large->armed);
    p.pendingWakeup=0;PosixTimersTickLocked(processes,1);
    CHECK(pendingCount==1 && !p.pendingWakeup && large->overrun==0 && large->interval==uint64_t(INT64_MAX));
    CHECK(PosixTimerDequeue(&p,bit,&info)==10 && !pendingCount);
    monotonicNs=beforeHorizon;
    CHECK(PosixTimerDelete(&p,id)==0 && !pendingCount && !usedCount);
}
}

void intervalTimers() {
    using namespace brook;
    constexpr uint64_t alrm=1ULL<<13,vtalrm=1ULL<<25,prof=1ULL<<26,ms=1000000;
    auto tick=[](Process* const* list,uint32_t n){PosixTimersTickLocked(list,n);};
    Process p{};init(p,20);Process peer{};init(peer,21);
    peer.tgid=p.tgid;peer.isThread=true;peer.leaderIncarnation=p.incarnation;
    g_perCpu[0].currentProcess=&p;
    Process* both[]={&p,&peer};
    IntervalTimerValue v{};
    CHECK(IntervalTimerSet(&p,3,ms,0,&v)==-22 && IntervalTimerGet(&p,-1,&v)==-22);
    // Unarmed: every gate the hot paths test is clear.
    CHECK(!realArmedCount && !g_intervalCpuTimersArmed && !IntervalTimerCpuArmed());
    // Process-wide: a sibling thread observes and cancels the leader's timer.
    CHECK(IntervalTimerSet(&p,ItimerReal,3000*ms,0,&v)==0 && !v.valueNs && !v.intervalNs);
    CHECK(realArmedCount==1);
    CHECK(IntervalTimerGet(&peer,ItimerReal,&v)==0 && v.valueNs==3000*ms);
    CHECK(IntervalTimerSet(&peer,ItimerReal,0,0,&v)==0 && v.valueNs==3000*ms);
    CHECK(IntervalTimerGet(&p,ItimerReal,&v)==0 && !v.valueNs && !realArmedCount);
    // Not inherited by fork: a new group (or a reused TGID) starts disarmed.
    CHECK(IntervalTimerSet(&p,ItimerReal,3000*ms,0,nullptr)==0);
    Process child{};init(child,22);CHECK(IntervalTimerGet(&child,ItimerReal,&v)==0 && !v.valueNs);
    Process reused{};init(reused,20);reused.incarnation=p.incarnation+1000;
    CHECK(IntervalTimerGet(&reused,ItimerReal,&v)==0 && !v.valueNs);
    // Exec (group-kill cleanup by the surviving leader) preserves the timer.
    PosixTimersCleanup(&p,true);CHECK(IntervalTimerGet(&p,ItimerReal,&v)==0 && v.valueNs==3000*ms);
    // Overdue before the tick: reported as one microsecond, never zero.
    monotonicNs+=3000*ms;CHECK(IntervalTimerGet(&p,ItimerReal,&v)==0 && v.valueNs==1000);
    tick(both,2);CHECK(p.pendingWakeup && peer.pendingWakeup && pendingCount==1);
    CHECK(PosixTimerPending(&p)==alrm && PosixTimerPending(&peer)==alrm);
    SignalInfo info{};CHECK(PosixTimerDequeue(&peer,alrm,&info)==14 && info.si_code==0x80);
    CHECK(!pendingCount && !PosixTimerPending(&p) && !realArmedCount);
    // Periodic with SIGALRM blocked: stays expired (value 0) until dequeued,
    // then restarts on the original phase.
    p.sigMask=peer.sigMask=alrm;p.pendingWakeup=peer.pendingWakeup=0;
    CHECK(IntervalTimerSet(&p,ItimerReal,20*ms,1000*ms,nullptr)==0);
    monotonicNs+=120*ms;tick(both,2);
    CHECK(p.pendingWakeup && pendingCount==1); // wakes synchronous waiters
    CHECK(IntervalTimerGet(&p,ItimerReal,&v)==0 && !v.valueNs && v.intervalNs==1000*ms);
    monotonicNs+=5*ms;tick(both,2);CHECK(pendingCount==1); // coalesced
    CHECK(PosixTimerPending(&p)==alrm); // blocked signals stay visible to sigtimedwait
    CHECK(PosixTimerDequeue(&p,alrm,&info)==14 && info.si_code==0x80);
    CHECK(IntervalTimerGet(&p,ItimerReal,&v)==0 && v.valueNs==(1000-105)*ms && realArmedCount==1);
    // Ignored and unblocked: discarded at expiry and not restarted.
    CHECK(IntervalTimerSet(&p,ItimerReal,0,0,nullptr)==0);
    p.sigMask=peer.sigMask=0;signalHandler=1;p.pendingWakeup=peer.pendingWakeup=0;
    CHECK(IntervalTimerSet(&p,ItimerReal,20*ms,1000*ms,nullptr)==0);
    monotonicNs+=120*ms;tick(both,2);
    CHECK(!pendingCount && !p.pendingWakeup && !peer.pendingWakeup && !PosixTimerPending(&p));
    CHECK(IntervalTimerGet(&p,ItimerReal,&v)==0 && !v.valueNs && v.intervalNs==1000*ms);
    // A kill()-delivered SIGALRM dequeue restarts the expired periodic timer.
    signalHandler=0x1234;IntervalTimerAlarmConsumed(&p);
    CHECK(IntervalTimerGet(&p,ItimerReal,&v)==0 && v.valueNs==(1000-100)*ms);
    // ...and absorbs a coalesced timer expiry that is still pending.
    monotonicNs+=900*ms;p.sigMask=alrm;tick(both,2);CHECK(pendingCount==1);
    IntervalTimerAlarmConsumed(&p);CHECK(!pendingCount && IntervalTimerGet(&p,ItimerReal,&v)==0 && v.valueNs==1000*ms);
    p.sigMask=0;
    // Claim path used by IRQ-return delivery.
    CHECK(IntervalTimerSet(&p,ItimerReal,ms,0,nullptr)==0);monotonicNs+=ms;tick(both,2);
    KernelSigaction action{};signalHandler=0x5678;
    CHECK(PosixTimerClaimCaught(&p,14,&info,&action) && action.handler==0x5678 && info.si_code==0x80 && !pendingCount);
    signalHandler=0x1234;
    // alarm-style replacement returns the previous value.
    CHECK(IntervalTimerSet(&p,ItimerReal,5000*ms,0,nullptr)==0);monotonicNs+=1500*ms;
    CHECK(IntervalTimerSet(&p,ItimerReal,0,0,&v)==0 && v.valueNs==3500*ms && !realArmedCount);
    // CPU timers: VIRTUAL counts user ticks only; PROF counts all ticks and
    // both reload from their interval at expiry.
    CHECK(IntervalTimerSet(&p,ItimerVirtual,2*ms,5*ms,nullptr)==0);
    CHECK(IntervalTimerSet(&peer,ItimerProf,3*ms,0,nullptr)==0);
    CHECK(g_intervalCpuTimersArmed==2 && IntervalTimerCpuArmed());
    IntervalTimerChargeTick(&peer,false,ms);
    CHECK(IntervalTimerGet(&p,ItimerVirtual,&v)==0 && v.valueNs==2*ms && v.intervalNs==5*ms);
    CHECK(IntervalTimerGet(&p,ItimerProf,&v)==0 && v.valueNs==2*ms);
    IntervalTimerChargeTick(&p,true,ms);IntervalTimerChargeTick(&peer,true,ms);
    CHECK(IntervalTimerGet(&p,ItimerVirtual,&v)==0 && v.valueNs==5*ms && pendingCount==2);
    CHECK(IntervalTimerGet(&p,ItimerProf,&v)==0 && !v.valueNs && g_intervalCpuTimersArmed==1);
    CHECK(PosixTimerPending(&p)==(vtalrm|prof));
    p.pendingWakeup=peer.pendingWakeup=0;tick(both,2);CHECK(p.pendingWakeup && peer.pendingWakeup);
    CHECK(PosixTimerDequeue(&p,prof,&info)==27 && PosixTimerDequeue(&peer,vtalrm,&info)==26 && !pendingCount);
    // SIG_DFL and unblocked: handed to the ordinary sender (group kill) once,
    // with a reference, rather than left pending for a syscall return.
    signalHandler=0;p.sigMask=peer.sigMask=0;
    CHECK(IntervalTimerSet(&p,ItimerReal,ms,0,nullptr)==0);monotonicNs+=ms;
    const auto refsBefore=p.refCount+peer.refCount;tick(both,2);
    Process* fatal[4]{};int signals[4]{};
    CHECK(!pendingCount && PosixTimersTakeFatal(fatal,signals,4)==1 && signals[0]==14);
    CHECK((fatal[0]==&p || fatal[0]==&peer) && p.refCount+peer.refCount==refsBefore+1);
    --fatal[0]->refCount;CHECK(PosixTimersTakeFatal(fatal,signals,4)==0);
    // Blocked default-action signals stay pending for sigtimedwait.
    p.sigMask=peer.sigMask=alrm;CHECK(IntervalTimerSet(&p,ItimerReal,ms,0,nullptr)==0);monotonicNs+=ms;tick(both,2);
    CHECK(pendingCount==1 && PosixTimersTakeFatal(fatal,signals,4)==0);
    CHECK(PosixTimerDequeue(&p,alrm,&info)==14);p.sigMask=peer.sigMask=0;signalHandler=0x1234;
    // Kernel threads are never charged.
    Process kernel{};init(kernel,23);kernel.isKernelThread=true;IntervalTimerChargeTick(&kernel,true,ms);
    // Group end clears everything, including pending expiries.
    IntervalTimerChargeTick(&p,true,5*ms);CHECK(pendingCount==1);
    CHECK(IntervalTimerSet(&p,ItimerReal,1000*ms,0,nullptr)==0);
    p.asLiveThreads=0;PosixTimersCleanup(&p,false);
    CHECK(!pendingCount && !realArmedCount && !g_intervalCpuTimersArmed);
    CHECK(IntervalTimerGet(&p,ItimerVirtual,&v)==0 && !v.valueNs && !v.intervalNs);
    // A dead group's armed timer is discarded at expiry and leaves no state.
    init(p,20);CHECK(IntervalTimerSet(&p,ItimerReal,ms,ms,nullptr)==0);
    p.state=ProcessState::Terminated;monotonicNs+=ms;Process* alone[]={&p};tick(alone,1);
    CHECK(!pendingCount && !realArmedCount);
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
    intervalTimers();
    {
        Process p{};init(p);const auto id=arm(p);expire(p);SignalInfo info{};KernelSigaction action{};
        signalHandler=1;CHECK(!PosixTimerClaimCaught(&p,10,&info,&action) && pendingCount==1);
        signalHandler=0x5678;CHECK(PosixTimerClaimCaught(&p,10,&info,&action));
        CHECK(action.handler==0x5678 && info.si_signo==10 && info.si_code==-2 && !pendingCount);
        CHECK(!PosixTimerClaimCaught(&p,10,&info,&action));CHECK(!PosixTimerDelete(&p,id));
    }
    {
        Process p{};init(p);const auto id=arm(p);expire(p);SignalInfo info{};KernelSigaction action{};
        std::atomic<bool> entered{false},competing{false};int stolen=-1;
        actionReadHook=[&]{CHECK(__atomic_load_n(&timerLock.next,__ATOMIC_ACQUIRE)==
                                 __atomic_load_n(&timerLock.serving,__ATOMIC_ACQUIRE)+1);
            entered=true;while(!competing.load())std::this_thread::yield();
            while(__atomic_load_n(&timerLock.next,__ATOMIC_ACQUIRE)==
                  __atomic_load_n(&timerLock.serving,__ATOMIC_ACQUIRE)+1)std::this_thread::yield();};
        std::thread other([&]{while(!entered.load())std::this_thread::yield();signalHandler=0x5678;competing=true;
            SignalInfo discarded{};stolen=PosixTimerDequeue(&p,1ULL<<9,&discarded);});
        CHECK(PosixTimerClaimCaught(&p,10,&info,&action));other.join();actionReadHook={};
        CHECK(!stolen && action.handler==0x5678 && info.si_signo==10 && info.si_code==-2);
        CHECK(!PosixTimerDelete(&p,id));
    }
    std::printf("POSIX timer clock: %u failures\n", failures);
    return failures ? 1 : 0;
}
