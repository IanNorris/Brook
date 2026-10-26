#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

// Compile the production adapter, replacing only interrupt/registry mechanics.
namespace fixture {
constexpr unsigned SCHED_MAX_PIDS = 16;
constexpr uint16_t SCHED_PID_NONE = 0xffff;
enum class ProcessState { Ready, Running, Blocked, Stopped, Terminated };
struct Process {
    uint16_t pid;
    uint64_t incarnation;
    uint8_t schedPriority;
    ProcessState state = ProcessState::Ready;
    int runningOnCpu = -1;
    int cpuAffinity = -1;
};
Process* g_pidToProcess[SCHED_MAX_PIDS]{};
struct Lock { bool held = false; } g_readyLock;
uint64_t SchedLockAcquire(Lock& lock) { assert(!lock.held); lock.held = true; return 0; }
void SchedLockRelease(Lock& lock, uint64_t flags) { assert(lock.held && !flags); lock.held = false; }
bool ProcessCanRunOnCpu(Process* process, unsigned cpu) {
    return process->cpuAffinity == -1 || process->cpuAffinity == static_cast<int>(cpu);
}
#include "scheduler_pi.inc"
}

int main() {
    using namespace fixture;
    Process owner{1, 11, 3}, donor{2, 22, 0}, middle{3, 33, 2}, peer{4, 44, 3};
    donor.state = ProcessState::Blocked;
    for (auto* task : {&owner, &donor, &middle, &peer}) g_pidToProcess[task->pid] = task;
    assert(PiCandidateLocked(0) == SCHED_PID_NONE); // Default policy untouched.
    SchedulerSetPiDonation(&owner, 0);
    assert(g_piDispatchCount == 1 && PiCandidateLocked(0) == owner.pid);
    assert(PiEffectiveRankLocked(&owner) == 0);
    owner.runningOnCpu = 1;
    assert(PiCandidateLocked(0) == middle.pid); // Never dispatch one task twice.
    owner.runningOnCpu = -1;
    owner.cpuAffinity = 1;
    assert(PiCandidateLocked(0) == middle.pid && PiCandidateLocked(1) == owner.pid);
    owner.cpuAffinity = -1;
    SchedulerSetPiDonation(&peer, 0);
    assert(g_piDispatchCount == 2);
    g_piDispatchCursor = owner.pid;
    assert(PiCandidateLocked(0) == peer.pid);
    g_piDispatchCursor = peer.pid;
    assert(PiCandidateLocked(0) == owner.pid);
    // An unrelated higher-priority runnable task outranks a lower donation.
    SchedulerSetPiDonation(&owner, 1);
    SchedulerSetPiDonation(&peer, 255);
    donor.state = ProcessState::Ready;
    assert(PiCandidateLocked(0) == donor.pid);
    donor.state = ProcessState::Blocked;
    assert(PiCandidateLocked(0) == owner.pid);
    owner.state = ProcessState::Stopped;
    assert(PiCandidateLocked(0) == middle.pid);
    owner.state = ProcessState::Terminated;
    SchedulerSetPiDonation(&owner, 0);
    assert(g_piDispatchCount == 0);
    owner.state = ProcessState::Ready;
    SchedulerSetPiDonation(&owner, 0);
    Process replacement{1, 999, 3};
    g_pidToProcess[1] = &replacement;
    assert(PiEffectiveRankLocked(&replacement) == 3); // PID reuse cannot inherit.
    SchedulerSetPiDonation(&owner, 255);
    assert(g_piDispatchCount == 0 && PiCandidateLocked(0) == SCHED_PID_NONE);
    assert(PiEffectiveRankLocked(&owner) == 3);
    SchedulerSetPiDonation(&replacement, 3); // No useful donation.
    assert(g_piDispatchCount == 0);
    std::puts("test_pi_dispatch: ALL PASS");
}
