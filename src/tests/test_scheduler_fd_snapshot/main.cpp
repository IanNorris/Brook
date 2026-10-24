#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include "../../kernel/src/fd_table.h"

namespace brook {
enum class ProcessState { Ready, Terminated };
struct Process {
    uint16_t pid;
    uint64_t incarnation;
    ProcessState state;
    FdEntry* fds;
    SpinLock fdLock;
};
struct FdSnapshot { uint8_t type; uint32_t flags; uint64_t seekPos; char path[64]; };
static Process* g_allProcesses[2];
static uint32_t g_processCount;
static SpinLock g_allProcLock;
static SpinLock* ProcessFdLock(Process* p) { return &p->fdLock; }
static uint64_t SchedLockAcquire(SpinLock& lock) { SpinLockAcquire(&lock); return 0; }
static void SchedLockRelease(SpinLock& lock, uint64_t) { SpinLockRelease(&lock); }
static Process* owner;
static unsigned yields, releaseAfter;
static void (*onRelease)();
static void SchedulerYield()
{
    // Deterministically model the scheduler work needed to resume the
    // preempted FD-lock owner. This acquisition must succeed before it resumes.
    assert(SpinLockTryAcquire(&g_allProcLock));
    SpinLockRelease(&g_allProcLock);
    if (++yields == releaseAfter) {
        SpinLockRelease(ProcessFdLock(owner));
        if (onRelease) onRelease();
    }
}
#include "../../kernel/src/scheduler_fd_snapshot.inc"
}

int main()
{
    using namespace brook;
    FdEntry slots[MAX_FDS]{};
    Process process{42, 123, ProcessState::Ready, slots, {}};
    g_allProcesses[0] = &process; g_processCount = 1; owner = &process;
    slots[7].type = FdType::Socket; slots[7].statusFlags = 0x800; slots[7].seekPos = 19;
    FdSnapshot snapshot{}; int fd = -1;
    assert(SchedulerGetFdInfo(42, 7, &snapshot, 123));
    assert(snapshot.type == static_cast<uint8_t>(FdType::Socket));
    assert(snapshot.flags == 0x800 && snapshot.seekPos == 19);
    assert(SchedulerGetFdByIndex(42, 0, &fd, &snapshot, 123) && fd == 7);
    assert(!SchedulerGetFdByIndex(42, 1, &fd, &snapshot, 123));
    assert(!SchedulerGetFdInfo(42, -1, &snapshot, 123));
    assert(!SchedulerGetFdInfo(42, MAX_FDS, &snapshot, 123));
    assert(!SchedulerGetFdInfo(42, 8, &snapshot, 123));
    assert(!SchedulerGetFdInfo(42, 7, &snapshot, 124));
    assert(!SchedulerGetFdInfo(99, 7, &snapshot, 123));

    for (bool byIndex : {false, true}) {
        SpinLockAcquire(&process.fdLock); yields = 0; releaseAfter = 3;
        assert(byIndex ? SchedulerGetFdByIndex(42, 0, &fd, &snapshot, 123)
                       : SchedulerGetFdInfo(42, 7, &snapshot, 123));
        assert(yields == 3);
    }
    // Removal while yielding must not leave a pointer into a detached process.
    SpinLockAcquire(&process.fdLock); yields = 0; releaseAfter = 1;
    onRelease = [] { g_processCount = 0; };
    assert(!SchedulerGetFdInfo(42, 7, &snapshot, 123));
    g_processCount = 1;
    // PID reuse and termination must be revalidated on every retry, for both
    // exact-FD lookup and directory enumeration.
    SpinLockAcquire(&process.fdLock); yields = 0;
    onRelease = [] { ++g_allProcesses[0]->incarnation; };
    assert(!SchedulerGetFdByIndex(42, 0, &fd, &snapshot, 123));
    SpinLockAcquire(&process.fdLock); yields = 0;
    onRelease = [] { g_allProcesses[0]->state = ProcessState::Terminated; };
    assert(!SchedulerGetFdInfo(42, 7, &snapshot, 124));
    // Preserve the existing incarnation=0 diagnostic semantics.
    assert(SchedulerGetFdInfo(42, 7, &snapshot, 0));
    assert(SpinLockTryAcquire(&g_allProcLock)); SpinLockRelease(&g_allProcLock);
    assert(SpinLockTryAcquire(&process.fdLock)); SpinLockRelease(&process.fdLock);
    puts("scheduler FD snapshots: contention, retry, removal and incarnation checks passed");
}
