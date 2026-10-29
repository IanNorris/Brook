// Compile the actual scheduler birth/terminal paths with inert policy, registry
// and interrupt adapters. ProcessFs/VFS storage and membership remain real.
namespace scheduler_membership_fixture {
using namespace brook;
struct Lock { bool held=false; };
Lock g_readyLock,g_allProcLock,g_pidLock;
uint64_t SchedLockAcquire(Lock& lock) {assert(!lock.held);lock.held=true;++schedulerLockDepth;return 0;}
void SchedLockReleaseRaw(Lock& lock) {assert(lock.held);lock.held=false;--schedulerLockDepth;}
void SchedLockRelease(Lock& lock,uint64_t flags) {assert(!flags);SchedLockReleaseRaw(lock);}
[[noreturn]] void KernelPanic(const char*,...) {std::abort();}
void KernelThreadTrampoline() {}
void ForkChildTrampoline() {}
void ProcessTrampoline() {}
constexpr unsigned SCHED_MAX_PIDS=MAX_PROCESSES;
Process* g_pidToProcess[MAX_PROCESSES]{};
Process* g_allProcesses[MAX_PROCESSES]{};
uint32_t g_processCount=0;uint64_t g_totalForks=0;
struct Policy {void InitProcess(void*,uint16_t,int) {assert(g_readyLock.held);}};
Policy policy;Policy* g_schedOps=&policy;void* g_schedState=nullptr;
struct Runtime {template<class... Args> void Register(Args...) {assert(g_readyLock.held);}} g_runtime;
struct Ids {void Publish(uint16_t) {assert(g_pidLock.held);}} g_processIds;
bool queued=false;
void ReadyQueueInsertLocked(Process*) {assert(g_readyLock.held);queued=true;}
void ReadyQueueRemoveLocked(Process*) {assert(g_readyLock.held);queued=false;}
void SchedulerRecordAsLive(Process*,uint16_t,uint16_t,uint32_t,uint32_t,int32_t,int) {assert(g_allProcLock.held);}
void KickIdleCpu() {assert(!schedulerLockDepth);}
void ReparentDeadGroupLocked(uint16_t) {assert(g_allProcLock.held);}
void PosixTimersCleanup(Process*,bool) {assert(!schedulerLockDepth);}
void SysvSemExit(Process*) {assert(!schedulerLockDepth);}
void FutexCancelProcess(Process*) {assert(!schedulerLockDepth);}
void ProcessQueueRobustExit(Process*) {assert(!schedulerLockDepth);}
void SchedulerUnblock(Process*) {assert(!schedulerLockDepth);}
Process* current=nullptr;
Process* SchedulerCurrentProcess() {return current;}
void SchedulerMarkTerminated(Process*,int);
#include "../../kernel/src/scheduler_process_birth.inc"
#include "../../kernel/src/scheduler_process_terminal.inc"

void test(Process& parent) {
    Process stillborn{},leader{};AddressSpaceState space{};
    assert(!ProcessFsInherit(&stillborn,&parent,false));
    assert(!ProcessFsIsolateMounts(&stillborn));
    FsSnapshot observer{};assert(!ProcessFsSnapshot(&stillborn,&observer));
    auto* ns=observer.mountNamespace;
    stillborn.addressSpace=&space;stillborn.pid=41;stillborn.tgid=40;
    stillborn.isThread=true;stillborn.threadLeader=&leader;
    leader.magic=PROCESS_MAGIC;leader.pid=leader.tgid=40;leader.incarnation=42;
    stillborn.leaderIncarnation=42;leader.tgidExiting=true;
    leader.parentPid=71;stillborn.parentPid=99; // snapshot preceded reparenting
    SchedulerAddProcess(&stillborn);
    assert(stillborn.state==ProcessState::Terminated && stillborn.reapable && !queued);
    assert(stillborn.parentPid==71);
    assert(!stillborn.mountNamespaceMember && ns->life.IsRetired());
    assert(g_processCount==1 && g_allProcesses[0]==&stillborn && leader.asLiveThreads==1);
    // Repeated termination/final cleanup cannot drop membership a second time.
    SchedulerMarkTerminated(&stillborn,99);ProcessFsClear(&stillborn);
    VfsDrainMountCleanup();FsSnapshotRelease(&observer);VfsDrainMountCleanup();
    g_pidToProcess[41]=nullptr;g_allProcesses[0]=nullptr;g_processCount=0;

    // CLONE_VM without CLONE_THREAD: prepared before selection, published
    // afterward. The AS latch must catch this regardless of isThread.
    Process lateClone{};assert(!ProcessFsInherit(&lateClone,&parent,false));
    assert(!ProcessFsIsolateMounts(&lateClone));
    assert(!ProcessFsSnapshot(&lateClone,&observer));ns=observer.mountNamespace;
    lateClone.addressSpace=&space;lateClone.pid=lateClone.tgid=42;
    lateClone.incarnation=100;lateClone.isThread=false;
    space.oomTerminating=true;
    SchedulerAddProcess(&lateClone);
    assert(lateClone.state==ProcessState::Terminated && lateClone.reapable && !queued);
    assert(!lateClone.mountNamespaceMember && ns->life.IsRetired());
    ProcessFsClear(&lateClone);VfsDrainMountCleanup();FsSnapshotRelease(&observer);VfsDrainMountCleanup();
    g_pidToProcess[42]=nullptr;g_allProcesses[0]=nullptr;g_processCount=0;space.oomTerminating=false;

    Process deferred{};assert(!ProcessFsInherit(&deferred,&parent,false));
    assert(!ProcessFsIsolateMounts(&deferred));assert(!ProcessFsSnapshot(&deferred,&observer));
    ns=observer.mountNamespace;deferred.state=ProcessState::Blocked;deferred.runningOnCpu=-1;
    deferred.kernelOperationProtected=true;deferred.kernelRegionDepth=1;deferred.interruptibleWait=true;
    SchedulerMarkTerminated(&deferred,17);SchedulerMarkTerminated(&deferred,23);
    assert(deferred.state==ProcessState::Blocked && deferred.terminalPending && deferred.pendingWaitStatus==17);
    assert(deferred.mountNamespaceMember==ns && ns->life.Published());
    current=&deferred;assert(!SchedulerKernelOperationBoundary());
    assert(deferred.mountNamespaceMember==ns && ns->life.Published());
    deferred.kernelRegionDepth=0;assert(SchedulerKernelOperationBoundary());
    assert(deferred.state==ProcessState::Terminated && deferred.waitStatus==17 && deferred.reapable);
    assert(!deferred.mountNamespaceMember && ns->life.IsRetired() && !deferred.refCount);
    SchedulerMarkTerminated(&deferred,42);assert(deferred.waitStatus==17 && !deferred.refCount);
    ProcessFsClear(&deferred);VfsDrainMountCleanup();FsSnapshotRelease(&observer);VfsDrainMountCleanup();current=nullptr;
}
} // namespace scheduler_membership_fixture
