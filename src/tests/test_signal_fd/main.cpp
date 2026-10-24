#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <functional>
#include <thread>
#define BROOK_HOST_TEST 1
#include "../../kernel/src/signal_fd.h"
#include "../../kernel/src/spinlock.h"
namespace brook {

constexpr unsigned MAX_PROCESSES=1024;
enum class ProcessState {Running,Stopped,Terminated};
struct SignalInfo {int32_t si_signo,si_errno,si_code,pad;uint8_t _data[112];};
struct Process {
    uint32_t pid,tgid,uid=1000;uint16_t parentPid=0;bool isThread=false;uint64_t leaderIncarnation=1;uint64_t incarnation=1,sigPending=0,sigMask=0,wakeupTick=0;
    bool terminalPending=false,continuedPending=false,tgidExiting=false;int waitStatus=0,stopSignal=0;
    ProcessState state=ProcessState::Running;Process* threadLeader=nullptr;
    struct Sender{int32_t pid;uint32_t uid;int32_t code;} sigSenders[64]{};
    std::atomic<unsigned> regions{0};std::mutex mutex;std::condition_variable cv;bool wake=false;
    explicit Process(unsigned id):pid(id),tgid(id){}
};
static void ProcessSignalInfoToUser(Process*,SignalInfo*) {} // identity namespace fixture
static std::atomic<int> allocations{0};static bool failAllocation=false,failCopy=false;
static void* kmalloc(uint64_t n){if(failAllocation)return nullptr;void* p=std::malloc(n);if(p)++allocations;return p;}
static void kfree(void* p){if(p){--allocations;std::free(p);}}
static uint64_t ApicTickCount(){return 100;}
static uint64_t PosixTimerPending(Process*){return 0;}
static bool PosixTimerDequeue(Process*,uint64_t,SignalInfo*){return false;}
static bool CopyToUser(uint64_t address,const void* source,uint64_t count){if(failCopy)return false;std::memcpy(reinterpret_cast<void*>(address),source,count);return true;}
static void SchedulerKernelRegionEnter(Process* p){++p->regions;}
static void SchedulerKernelRegionLeave(Process* p){--p->regions;}
static void SchedulerBlockInterruptible(Process* p){std::unique_lock<std::mutex> hold(p->mutex);p->cv.wait_for(hold,std::chrono::milliseconds(10),[&]{return p->wake;});p->wake=false;}
static void wake(Process& p){std::lock_guard<std::mutex> hold(p.mutex);p.wake=true;p.cv.notify_all();}
#define BROOK_SIGNAL_IDENTITY_TEST 1
static std::function<void()> identityHook;
static void SignalIdentityTransitionTest(){if(identityHook)identityHook();}
#include "../../kernel/src/signal_pending.inc"
static std::mutex g_readyLock,g_allProcLock;
static uint64_t SchedLockAcquire(std::mutex& lock){lock.lock();return 0;}
static void SchedLockRelease(std::mutex& lock,uint64_t){lock.unlock();}
static uint32_t ThisCpu(){return 0;}
static uint64_t RuntimeNow(uint32_t){return 0;}
static void RuntimeChargeLocked(uint32_t,uint64_t){}
static unsigned releasedMembership=0;
static void ReleaseThreadAddressSpaceLocked(Process*){++releasedMembership;}
static struct {void Rebind(uint32_t,uint64_t,uint32_t,uint64_t){}} g_runtime;
static Process* g_allProcesses[MAX_PROCESSES]{};
static uint32_t g_processCount=0;
#include "../../kernel/src/scheduler_subreaper.inc"
#include "../../kernel/src/scheduler_exec_identity.inc"
#include "../../kernel/src/signal_fd_service.inc"
}
using namespace brook;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(0)
static void send(Process& p,int signo,int sender=99){SignalInfo info{};info.si_signo=signo;info.si_code=0;std::memcpy(info._data,&sender,4);uint32_t uid=123;std::memcpy(info._data+4,&uid,4);QueueSignal(&p,signo,&info);wake(p);}
static int64_t read(Process& p,SignalFd* fd,SignalFdInfo* out,unsigned count=1,bool nonblock=true){return SignalFdRead(&p,fd,reinterpret_cast<uint64_t>(out),uint64_t(count)*128,nonblock);}
int main(){
    {
        Process reaper(101),dying(102),thread(103),child(104),execing(105),pending(106);
        reaper.incarnation=500;reaper.tgidExiting=true; // exec birth latch, not death
        dying.parentPid=101;dying.state=ProcessState::Terminated;
        thread.tgid=101;thread.isThread=true;thread.leaderIncarnation=499;
        thread.parentPid=999; // a stale member cannot supply canonical ancestry
        g_allProcesses[0]=&reaper;g_allProcesses[1]=&dying;g_allProcesses[2]=&thread;g_processCount=3;
        CHECK(!SchedulerChildSubreaperControl(&reaper,true,true));
        CHECK(FindChildSubreaperLocked(102)==101);
        reaper.state=ProcessState::Terminated;thread.leaderIncarnation=500;
        CHECK(FindChildSubreaperLocked(102)==101); // surviving sibling is enough
        thread.state=ProcessState::Terminated;
        CHECK(FindChildSubreaperLocked(102)==0);
        reaper.state=ProcessState::Running;
        child.parentPid=101;
        execing.tgid=101;execing.isThread=true;execing.leaderIncarnation=500;
        g_allProcesses[3]=&child;g_allProcesses[4]=&execing;g_processCount=5;
        SchedulerCommitExecThreadIdentity(&execing);
        CHECK(execing.tgid==105 && child.parentPid==101);
        CHECK(SchedulerChildSubreaperControl(&reaper,false,false)==1);
        CHECK(SchedulerChildSubreaperControl(&execing,false,false)==1);
        Process last(107);last.tgid=101;last.isThread=true;last.leaderIncarnation=500;
        pending.tgid=101;pending.isThread=true;pending.leaderIncarnation=500;pending.terminalPending=true;
        reaper.state=ProcessState::Terminated;
        g_allProcesses[5]=&pending;g_allProcesses[6]=&last;g_processCount=7;
        TransferChildSubreaperExecLocked(&last);
        CHECK(child.parentPid==101); // pending is still a live old-group peer
        pending.state=ProcessState::Terminated;
        SchedulerCommitExecThreadIdentity(&last);
        CHECK(child.parentPid==107 && last.tgid==107);
        CHECK(SchedulerChildSubreaperControl(&last,false,false)==1);
        g_childSubreapers[101]={500,true};
        Process reused(101);reused.incarnation=501;g_allProcesses[0]=&reused;
        CHECK(SchedulerChildSubreaperControl(&reused,false,false)==0);
        g_processCount=0;releasedMembership=0;
    }
    Process parent(1),child(2);parent.sigMask=~uint64_t{0};child.sigMask=parent.sigMask;
    auto* fd=SignalFdCreate(~uint64_t{0});CHECK(fd && !(fd->mask&(1ULL<<8)) && !(fd->mask&(1ULL<<18)));
    SignalFdInfo info[2]{};CHECK(read(parent,fd,info)==-11);CHECK(SignalFdRead(&parent,fd,0,127,true)==-22);
    send(parent,10);CHECK(SignalFdReady(&parent,fd,5)==1 && !SignalFdReady(&child,fd,5));
    CHECK(read(child,fd,info)==-11);CHECK(read(parent,fd,info)==128);CHECK(info[0].signo==10 && info[0].pid==99 && info[0].uid==123);
    CHECK(!SignalFdReady(&parent,fd,1));
    SignalFdRef(fd);SignalFdSetMask(fd,1ULL<<11);send(parent,10);CHECK(read(parent,fd,info)==-11);
    send(parent,12);CHECK(read(parent,fd,info)==128 && info[0].signo==12);
    SignalFdSetMask(fd,~uint64_t{0});CHECK(read(parent,fd,info)==128 && info[0].signo==10);SignalFdUnref(fd);
    // Standard signal coalescing retains the first sender; multiple records fit.
    send(parent,10,55);send(parent,10,66);send(parent,12,77);
    CHECK(read(parent,fd,info,2)==256 && info[0].pid==55 && info[1].pid==77);
    // Child metadata survives changing/reaping the child object and coalesces.
    child.state=ProcessState::Terminated;child.waitStatus=37<<8;child.continuedPending=true;
    ProcessQueueChildSignal(&parent,&child);child.waitStatus=9;ProcessQueueChildSignal(&parent,&child);
    CHECK(read(parent,fd,info)==128 && info[0].signo==17 && info[0].pid==2 && info[0].code==1 && info[0].status==37);
    ProcessQueueChildSignal(&parent,&child);CHECK(read(parent,fd,info)==128 && info[0].code==2 && info[0].status==9);
    child.state=ProcessState::Stopped;child.stopSignal=20;ProcessQueueChildSignal(&parent,&child);
    CHECK(read(parent,fd,info)==128 && info[0].code==5 && info[0].status==20);
    child.state=ProcessState::Running;ProcessQueueChildSignal(&parent,&child);
    CHECK(read(parent,fd,info)==128 && info[0].code==6 && info[0].status==18);
    // A worker sees shared SIGCHLD but cannot consume leader-private tgkill bits.
    Process worker(3);worker.tgid=parent.tgid;worker.isThread=true;worker.leaderIncarnation=parent.incarnation;
    ProcessQueueChildSignal(&parent,&child);send(parent,10,44);
    CHECK(SignalFdReady(&worker,fd,1)==1);CHECK(read(worker,fd,info)==128 && info[0].signo==17);
    CHECK(read(worker,fd,info)==-11);CHECK(read(parent,fd,info)==128 && info[0].signo==10);
    ProcessQueueChildSignal(&parent,&child);SchedulerCommitExecThreadIdentity(&worker);
    CHECK(read(worker,fd,info)==128 && info[0].signo==17 && !SignalFdReady(&parent,fd,1));
    // Readers and parent notification cannot observe the half-written identity.
    Process transitioning(4);transitioning.tgid=parent.tgid;transitioning.isThread=true;
    transitioning.leaderIncarnation=parent.incarnation;ProcessQueueChildSignal(&parent,&child);
    std::atomic<unsigned> started{0},finished{0};std::thread observer,notifier;
    identityHook=[&]{
        observer=std::thread([&]{++started;CHECK(ProcessPendingSignals(&transitioning)&(1ULL<<16));++finished;});
        notifier=std::thread([&]{++started;std::lock_guard<std::mutex> hold(g_allProcLock);ProcessQueueChildSignal(&transitioning,&child);++finished;});
        while(started.load()!=2)std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));CHECK(finished==0);
    };
    SchedulerCommitExecThreadIdentity(&transitioning);identityHook={};observer.join();notifier.join();
    CHECK(releasedMembership==2 && finished==2 && transitioning.tgid==transitioning.pid && !transitioning.isThread);
    CHECK(read(transitioning,fd,info)==128 && info[0].signo==17 && !SignalFdReady(&parent,fd,1));
    // PID reuse never supplies an earlier incarnation's cached child record.
    ProcessQueueChildSignal(&parent,&child);parent.sigPending=0;++parent.incarnation;
    send(parent,17,88);CHECK(read(parent,fd,info)==128 && info[0].pid==88 && info[0].code==0);
    // Fault consumes the selected signal, matching the Linux dequeue boundary.
    send(parent,10);failCopy=true;CHECK(read(parent,fd,info)==-14);failCopy=false;CHECK(read(parent,fd,info)==-11);
    std::atomic<int64_t> result{-999};
    std::thread reader([&]{result=read(parent,fd,info,1,false);});
    while(!parent.regions.load())std::this_thread::yield();send(parent,10);reader.join();CHECK(result==128);
    std::thread killed([&]{result=read(parent,fd,info,1,false);});
    while(!parent.regions.load())std::this_thread::yield();__atomic_store_n(&parent.terminalPending,true,__ATOMIC_RELEASE);wake(parent);killed.join();CHECK(result==-4 && parent.regions==0);
    uint8_t payload[112]{};uint32_t timer=7,overrun=3;uint64_t value=0x12345678abcdefULL;
    std::memcpy(payload,&timer,4);std::memcpy(payload+4,&overrun,4);std::memcpy(payload+8,&value,8);
    auto record=SignalFdEncode(14,0,-2,payload);CHECK(record.tid==7 && record.overrun==3 && record.pointer==value);
    SignalFdUnref(fd);failAllocation=true;CHECK(!SignalFdCreate(0));failAllocation=false;CHECK(allocations==0);
    std::puts("Signal fd service and child metadata: PASS");
}
