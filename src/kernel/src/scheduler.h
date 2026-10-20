#pragma once

#include "process.h"
#include "sched_ops.h"
#include "ready_latency.h"
#include "runtime_accounting.h"

struct KernelCpuEnv;

namespace brook {

// ---------------------------------------------------------------------------
// Scheduler — preemptive with pluggable policy and SMP support
// ---------------------------------------------------------------------------

// Initialise the scheduler. Creates the idle process for the BSP.
void SchedulerInit();

// Create an idle process for an AP. Must be called from BSP before AP starts.
void SchedulerInitApIdle(uint32_t cpuIndex);

// Set the KernelCpuEnv pointer for a CPU (for syscall stack updates).
void SchedulerSetCpuEnv(uint32_t cpuIndex, KernelCpuEnv* env);

// Add a process to the ready queue.
void SchedulerAddProcess(Process* proc);

// Remove a process from the ready queue.
void SchedulerRemoveProcess(Process* proc);

// Block the current process. Triggers an immediate reschedule.
void SchedulerBlock(Process* proc);
// Current-thread wait with terminal-cancellation wakeup. Caller must unregister
// its wait records and unwind resources after return, then reach the ordinary
// kernel-operation boundary. Never use for mutex/ownership-grant waits.
void SchedulerBlockInterruptible(Process* proc);

// Unblock a process — move it from Blocked back to Ready queue.
void SchedulerUnblock(Process* proc);
void SchedulerContinue(Process* proc);

// Called from the LAPIC timer ISR on each CPU. `allowPreempt` should be true
// only when the interrupted context was preemptible user mode; wakeups and idle
// dispatch still run when false.
struct TimerWakeToken;
void SchedulerTimerTick(bool allowPreempt, const TimerWakeToken* timerProbe=nullptr,bool periodicTimer=false);

// Yield the current timeslice voluntarily.
extern "C" void SchedulerYield();

// Block the current process for approximately `ms` milliseconds.  Uses the
// same wakeup-tick path as sys_nanosleep so other CPUs / processes can run
// while we wait.  Safe to call from kernel/driver context that has a
// current Process; no-op in early boot before the scheduler is ready.
extern "C" void SchedulerSleepMs(uint32_t ms);

// Trampoline for kernel threads — drains requeue, enables interrupts,
// reads fn/arg from the kernel stack, and calls fn(arg).
void KernelThreadTrampoline();

// Start the scheduler on the BSP — picks the first ready process and enters user mode.
[[noreturn]] void SchedulerStart();

// Start the scheduler on an AP — enters the scheduling loop.
[[noreturn]] void SchedulerStartAp();

// Terminate the current process and reschedule. Never returns.
[[noreturn]] void SchedulerExitCurrentProcess(int status, int terminationSignal = 0);
void SchedulerKillThreadGroup(uint16_t tgid, Process* caller, int exitStatus,
                              int terminationSignal = 0);
// Serialize terminal state publication against block/wake/dispatch transitions.
// Enter before taking a sleeping-lock guard; leave after releasing it. Entry
// and fatal-state publication are serialized by readyLock, never a lock guard.
void SchedulerKernelRegionEnter(Process* proc);
void SchedulerKernelRegionLeave(Process* proc);
// Only at a completed syscall/user-fault boundary, user-mode tick, or final exit.
// Returns true if terminal delivery was committed; caller must not return to user.
bool SchedulerKernelOperationBoundary();

// IRQ/lock-safe fatal signal request. Latches birth, marks all generation-matched
// members and releases snapshot holds without waiting for peer quiescence.
// Caller keeps anchor alive for this call; cooperative cancellation drains waits.
void SchedulerRequestFatalSignal(Process* anchor, int signal);
void SchedulerMarkTerminated(Process* proc, int waitStatus);
// Request a stop without yielding (for the syscall signal-return path).
void SchedulerMarkStopped(Process* proc, int signal = 19);
// Drop an owned wait reference and notify a parent if this made it reapable.
void SchedulerReleaseWaitReference(Process* proc);
// Registry-pinned lookup; caller must ProcessUnref the returned identity.
Process* SchedulerFindProcessReference(uint16_t pid);

// Resolve a timer target under the process registry lock; returns only a token.
bool SchedulerTimerTargetIncarnation(Process* owner, int32_t tid, uint64_t* incarnation);

// Get the current process running on this CPU (nullptr if none).
Process* SchedulerCurrentProcess();

// Get the number of processes in the run queue.
uint32_t SchedulerReadyCount();

// Allocate a unique PID.
uint16_t SchedulerAllocPid(); // Kernel/internal allocation; returns zero on exhaustion.
uint16_t SchedulerAllocUserPid(); // Holds a birth lease until publish or free.
// Lifecycle coordinator only: close after app-save/close handling, before the
// final user-task census. pending==0 alone does NOT imply users have exited.
uint32_t SchedulerSetUserBirthClosed(bool closed);
void SchedulerUserBirthStatus(bool* closed,uint32_t* pending);
void SchedulerFreePid(uint16_t pid);

// Snapshot a selected child event. Return 1 for an event, 0 for eligible
// children without an event, or -ECHILD. Options use Linux W* bit values.
// Consumption detaches a zombie atomically; destruction finishes before return.
struct ChildWaitSnapshot {
    int32_t pid, status, code;
    uint32_t uid;
    uint64_t userTicks, sysTicks;
};
int SchedulerWaitChild(Process* caller, int selector, uint32_t id,
                       uint32_t options, ChildWaitSnapshot* result);
void SchedulerDumpChildState(uint16_t parentPid, uint16_t targetPid);
// A job tag cannot match a reused PID. Counts include all descendant process
// groups; originalGroupTasks separately describes the initial POSIX group.
struct DebugJobSnapshot {
    uint32_t liveTasks, originalGroupTasks;
    bool leaderPresent, leaderExited;
};
DebugJobSnapshot SchedulerDebugJobSnapshot(uint64_t job, uint16_t leaderPid,
                                          uint64_t leaderIncarnation);
void SchedulerStop(Process* proc, int signal = 19);

// Find a non-terminated process whose name (basename, possibly suffixed
// by SpawnProcess with `_NN`) matches the given basename.  Used by the
// shell to make `run-once` idempotent — re-sourced launcher scripts
// must not double-spawn long-running helpers like waylandd.  Returns
// nullptr if no live match is found.
Process* SchedulerFindProcessByBaseName(const char* basename);

// Complete a destroyed thread's shared-AS hold and notify a now-waitable parent.
void SchedulerReleaseThreadAddressSpace(Process* thread);

// Snapshot of a process for procfs.
struct ProcessSnapshot {
    uint16_t pid;
    uint16_t parentPid;
    uint16_t tgid;
    uint64_t incarnation;
    uint64_t groupIncarnation;
    uint32_t threadCount;
    uint16_t pgid;
    uint16_t sid;
    ProcessState state;
    char name[32];
    uint64_t stackBase;
    uint64_t stackTop;
    uint64_t programBreak;
    int32_t runningOnCpu;
    uint64_t userTicks;
    uint64_t sysTicks;
    char exePath[256];
    char cwd[256];
};

// Take a snapshot of all processes.  Fills `out` with up to `maxCount` entries.
// Returns the number of entries written.
uint32_t SchedulerSnapshotProcesses(ProcessSnapshot* out, uint32_t maxCount);

// Compact diagnostic snapshot: one registry lock, no pointers, AS locks or
// per-task group scans. Task state/CPU counters may change concurrently.
struct DebugTaskSnapshot {
    uint16_t pid,parentPid,tgid;
    uint64_t incarnation,groupIncarnation,launchId,userTicks,sysTicks;
    uint32_t uid,euid,gid,egid;
    int32_t runningOnCpu;
    ProcessState state;
    bool kernel,systemService;
    char name[32];
};
uint32_t SchedulerSnapshotDebugTasks(DebugTaskSnapshot* out,uint32_t capacity,
                                   uint32_t* total,uint64_t* beginTick,uint64_t* endTick);

// Snapshot a single process by PID. Returns true if found.
bool SchedulerSnapshotProcess(uint16_t pid, ProcessSnapshot* out);

// Generation-bound live group lookup; tid=0 selects the lowest live TID at
// or above minTid. No Process pointer escapes the registry lock.
bool SchedulerSnapshotTask(uint16_t tgid, uint64_t groupIncarnation,
                           uint16_t tid, uint32_t minTid, ProcessSnapshot* out);
uint32_t SchedulerCountTasks(uint16_t tgid, uint64_t groupIncarnation);

// Return the PID of the Nth active process (0-indexed). Returns false if out of range.
bool SchedulerGetPidByIndex(uint32_t index, uint16_t* outPid);

// Cumulative scheduler stats for /proc/stat
uint64_t SchedulerGetTotalForks();
void SchedulerGetReapedTicks(uint64_t& userTicks, uint64_t& sysTicks);

// Get total and running process counts for /proc/loadavg
void SchedulerGetProcessCounts(uint32_t& total, uint32_t& running);

// Get EWMA load averages (each value * 1000 for fixed-point)
void SchedulerGetLoadAvg(uint32_t& avg1, uint32_t& avg5, uint32_t& avg15);

// Snapshot of an open fd for /proc/PID/fd/.
struct FdSnapshot {
    uint8_t  type;      // FdType as uint8_t
    uint32_t flags;     // statusFlags (O_* flags)
    uint64_t seekPos;
    char     path[64];  // dirPath or type name
};

// Get the Nth open fd for a process. Returns the fd number in outFd, info in outSnap.
// cookie is the iteration index (0, 1, 2, ...). Returns false when no more fds.
bool SchedulerGetFdByIndex(uint16_t pid, uint32_t index, int* outFd, FdSnapshot* outSnap, uint64_t incarnation = 0);

// Get info about a specific fd of a process. Returns false if fd not open.
bool SchedulerGetFdInfo(uint16_t pid, int fd, FdSnapshot* outSnap, uint64_t incarnation = 0);

// Get per-CPU tick counters for /proc/stat.
// Writes busy and idle tick counts for CPU `cpuIndex`.
void SchedulerGetCpuTicks(uint32_t cpuIndex, uint64_t& busyTicks, uint64_t& idleTicks);

// Return the process currently running on the given CPU (nullptr if none/invalid).
Process* SchedulerGetCpuProcess(uint32_t cpuIndex);

// Running-interval total; remote siblings are committed at their own timer/
// dispatch boundary. Includes kernel/IRQ work, excludes reported host steal.
bool SchedulerCpuTime(Process* proc, bool group, uint64_t* result);
void SchedulerCpuTimeRebind(Process* proc);
struct RuntimeStatus {
    RuntimeDiagnostics diagnostics{};
    uint64_t storageBytes = 0, stealCpuMask = 0;
    bool stableClock = false;
};
void SchedulerRuntimeStatus(RuntimeStatus* output);
struct PolicyServiceStatus {
    const char* name = "none";
    uint64_t stateBytes = 0, ledgerSinceSelectionNs = 0;
    SchedDebugInfo debug{};
    bool hasQueueDebug = false;
};
void SchedulerPolicyServiceStatus(PolicyServiceStatus* output);

// Register a scheduling policy. Called by scheduler modules during init().
// Required callbacks and the shared state bound are validated. A different
// table cannot replace an existing name. Registered modules must stay resident;
// the module loader does not yet have an unregister/quiescence contract.
// Providers of the table/name/callbacks are permanently retained before publish.
// Heap-owned tables and policy unregister/hot replacement are not supported.
extern "C" bool SchedulerRegisterPolicy(const SchedOps* ops);

// Switch to a registered policy by name. Returns true on success.
// Serializes policy state replacement with callbacks and task birth. Rebuilds
// only off-CPU Ready entries; running/blocked tasks retain initialized entries.
bool SchedulerSwitchPolicy(const char* name);

// Get the name of the currently active scheduling policy.
const char* SchedulerPolicyName();

// Runtime-off queue-entry-to-dispatch diagnostics. Copies under readyLock;
// callers format/send after release. No blocked-time or wakeup-IPI measurement.
void SchedulerReadyLatency(ReadyLatencyAction action, ReadyLatencySnapshot* output);
bool SchedulerReadyLatencyWatch(uint32_t pid);

// Panic-safe process enumeration — no locks, assumes all other CPUs halted.
uint32_t PanicGetProcessCount();
Process* PanicGetProcess(uint32_t index);

// Dump all thread states to serial (for debugging hangs).
void SchedulerDumpThreadStates();

// Bounded launch-feedback liveness query; excludes kernel worker threads.
bool SchedulerLaunchHasLiveProcesses(uint64_t launchId);

} // namespace brook
