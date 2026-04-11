#pragma once

#include "process.h"

namespace brook {

// ---------------------------------------------------------------------------
// Scheduler — preemptive round-robin
// ---------------------------------------------------------------------------

// Initialise the scheduler. Creates the idle process.
// Must be called after memory subsystem and LAPIC are ready.
void SchedulerInit();

// Add a process to the ready queue. The process must be in Ready state.
void SchedulerAddProcess(Process* proc);

// Remove a process from the ready queue (on exit or block).
void SchedulerRemoveProcess(Process* proc);

// Block the current process (e.g. nanosleep). Triggers an immediate reschedule.
// The caller must set proc->wakeupTick before calling if a timed wakeup is needed.
void SchedulerBlock(Process* proc);

// Unblock a process — move it from Blocked back to Ready queue.
void SchedulerUnblock(Process* proc);

// Called from the LAPIC timer ISR. Checks if the current timeslice has expired
// and performs a context switch if needed. Only preempts user-mode code.
// `interruptFrame` is the CPU-pushed interrupt frame on the kernel stack.
void SchedulerTimerTick();

// Yield the current timeslice voluntarily (e.g. after unblocking another process).
void SchedulerYield();

// Start the scheduler — picks the first ready process and enters user mode.
// This function never returns.
[[noreturn]] void SchedulerStart();

// Terminate the current process and reschedule. Never returns.
[[noreturn]] void SchedulerExitCurrentProcess(int status);

// Get the number of processes in the run queue (for debug).
uint32_t SchedulerReadyCount();

// Allocate a unique PID.
uint16_t SchedulerAllocPid();

} // namespace brook
