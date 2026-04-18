#pragma once

#include "process.h"
#include "sched_ops.h"

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

// Unblock a process — move it from Blocked back to Ready queue.
void SchedulerUnblock(Process* proc);

// Called from the LAPIC timer ISR on each CPU.
void SchedulerTimerTick();

// Yield the current timeslice voluntarily.
extern "C" void SchedulerYield();

// Trampoline for kernel threads — drains requeue, enables interrupts,
// reads fn/arg from the kernel stack, and calls fn(arg).
void KernelThreadTrampoline();

// Start the scheduler on the BSP — picks the first ready process and enters user mode.
[[noreturn]] void SchedulerStart();

// Start the scheduler on an AP — enters the scheduling loop.
[[noreturn]] void SchedulerStartAp();

// Terminate the current process and reschedule. Never returns.
[[noreturn]] void SchedulerExitCurrentProcess(int status);

// Get the current process running on this CPU (nullptr if none).
Process* SchedulerCurrentProcess();

// Get the number of processes in the run queue.
uint32_t SchedulerReadyCount();

// Allocate a unique PID.
uint16_t SchedulerAllocPid();

// Find a terminated child of parentPid. If pid == -1, any child; otherwise
// matches a specific child PID. Returns the child Process* or nullptr.
Process* SchedulerFindTerminatedChild(uint16_t parentPid, int64_t pid);
Process* SchedulerFindStoppedChild(uint16_t parentPid, int64_t pid);
void SchedulerStop(Process* proc);

// Reap (destroy) a terminated child process after wait4 collects its status.
void SchedulerReapChild(Process* child);

// Snapshot of a process for procfs.
struct ProcessSnapshot {
    uint16_t pid;
    uint16_t parentPid;
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
};

// Take a snapshot of all processes.  Fills `out` with up to `maxCount` entries.
// Returns the number of entries written.
uint32_t SchedulerSnapshotProcesses(ProcessSnapshot* out, uint32_t maxCount);

// Register a scheduling policy. Called by scheduler modules during init().
// Multiple policies can be registered; only the active one is used.
extern "C" void SchedulerRegisterPolicy(const SchedOps* ops);

// Switch to a registered policy by name. Returns true on success.
// Safe to call at runtime — migrates all processes to the new policy.
bool SchedulerSwitchPolicy(const char* name);

// Get the name of the currently active scheduling policy.
const char* SchedulerPolicyName();

// Panic-safe process enumeration — no locks, assumes all other CPUs halted.
uint32_t PanicGetProcessCount();
Process* PanicGetProcess(uint32_t index);

} // namespace brook
