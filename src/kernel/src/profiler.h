#pragma once

// profiler.h — Sampling profiler with stack trace capture + context-switch tracing
//
// Each LAPIC timer tick (~1ms per CPU), the profiler records the interrupted
// RIP, walks the RBP frame chain for a stack trace, captures the current PID,
// CPU index, and privilege ring into a per-CPU lock-free ring buffer.
// Additionally, every scheduler context switch emits a CS event so the drain
// thread can reconstruct per-process run/block timelines alongside the samples.
// A background kernel thread continuously drains events to serial output.
// A host-side script (profiler_to_speedscope.py) converts the text dump
// to Speedscope JSON for visualization.
//
// Serial format:
//   P  <tick_dec> <pid_hex> <cpu> <flags> <rip0_hex>;<rip1_hex>;...
//   CS <tick_dec> <cpu>     <old_pid_hex> <new_pid_hex>

#include <stdint.h>

namespace brook {

// Initialise profiler subsystem (creates kernel thread, does NOT start sampling).
void ProfilerInit();

// Start profiling for 'durationMs' milliseconds (0 = until ProfilerStop).
void ProfilerStart(uint32_t durationMs);

// Stop profiling early and flush remaining samples.
void ProfilerStop();

// Is the profiler currently sampling?
bool ProfilerIsRunning();

// Called from the LAPIC timer ISR on every CPU.
// interruptedRip: the RIP from the interrupt frame.
// interruptedCs:  the CS from the interrupt frame (to determine ring).
// interruptedRbp: the RBP at interrupt time (for frame pointer walking).
// Must be safe for ISR context: no locks, no memory allocation.
void ProfilerSample(uint64_t interruptedRip, uint64_t interruptedCs, uint64_t interruptedRbp);

// Called from the scheduler immediately before each context_switch().
// oldPid: PID of the process being switched away from (0xFFFF = idle).
// newPid: PID of the process being switched to.
// Must be safe for scheduler context: no locks, no memory allocation.
void ProfilerContextSwitch(uint16_t oldPid, uint16_t newPid);

} // namespace brook
