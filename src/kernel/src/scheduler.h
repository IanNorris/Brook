#pragma once

#include "process.h"

struct KernelCpuEnv;

namespace brook {

// ---------------------------------------------------------------------------
// Scheduler — preemptive round-robin with SMP support
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
void SchedulerYield();

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

} // namespace brook
