#pragma once

// profiler.h — Sampling profiler that captures RIP from timer ISR
//
// Each LAPIC timer tick (~1ms per CPU), the profiler records the interrupted
// RIP, current PID, CPU index, and privilege ring into a per-CPU lock-free
// ring buffer.  A background kernel thread periodically drains all buffers
// to a binary file on disk.  A host-side script converts the binary dump
// to Speedscope JSON for visualization.
//
// Binary format (per sample, 16 bytes):
//   uint32_t tick   — relative to profiler start (ms)
//   uint16_t pid    — process ID
//   uint8_t  cpu    — CPU index
//   uint8_t  flags  — bit 0: ring (0=kernel, 1=user)
//   uint64_t rip    — instruction pointer at interrupt time

#include <stdint.h>

namespace brook {

// Initialise profiler subsystem (creates kernel thread, does NOT start sampling).
void ProfilerInit();

// Start profiling for 'durationMs' milliseconds (0 = until ProfilerStop).
// Samples are written to /profiler.bin on disk.
void ProfilerStart(uint32_t durationMs);

// Stop profiling early and flush remaining samples.
void ProfilerStop();

// Called from the LAPIC timer ISR on every CPU.
// interruptedRip: the RIP from the interrupt frame.
// interruptedCs:  the CS from the interrupt frame (to determine ring).
// Must be safe for ISR context: no locks, no memory allocation.
void ProfilerSample(uint64_t interruptedRip, uint64_t interruptedCs);

} // namespace brook
