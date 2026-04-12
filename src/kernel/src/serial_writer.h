#pragma once

#include <stdint.h>

namespace brook {

// Initialise the async serial/TTY writer subsystem.
// Must be called after SchedulerInit() and before SchedulerStart().
// Creates a kernel thread that drains the ring buffer to serial + TTY.
void SerialWriterInit();

// Enqueue data for async output to serial and/or TTY.
// Returns immediately — data is copied into a ring buffer and drained
// by the writer thread. Safe to call from any context with interrupts enabled.
// `len` bytes from `buf` are enqueued; excess is silently dropped.
void SerialWriterEnqueue(const char* buf, uint32_t len);

} // namespace brook
