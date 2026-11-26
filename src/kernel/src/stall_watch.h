#pragma once
// Always-on stall watch (26 Nov 2 x 2 s desktop stall). Every LAPIC timer interrupt stamps its
// CPU's time, interrupted RIP and current PID (a few stores). A watcher thread wakes every
// 50 ms; when its own wake-up is more than 500 ms late, or on StallWatchTrigger (CE present
// timeout), it logs one line per online CPU: how long ago that CPU last took a timer IRQ (a
// large age = that CPU ran with IRQs off), the RIP it was at and the PID. Rate-limited to one
// dump per 5 s.
#include <stdint.h>

namespace brook {
void StallWatchTick(uint64_t interruptedRip);   // LAPIC timer ISR, every CPU
void StallWatchTrigger(const char* why);        // any context; logs from the watcher thread
void StallWatchStart();                          // once, after the scheduler runs
}
