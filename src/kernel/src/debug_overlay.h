#pragma once
// debug_overlay.h — Kernel console ring buffer + WM window.
//
// Captures kernel log output into a ring buffer. The kernel console window
// thread reads from this buffer and renders into its own VFB.

#include <stdint.h>

namespace brook {

// Initialise the kernel console ring buffer.
void DebugOverlayInit();

// Append text to the ring buffer (safe from any context).
void DebugOverlayPuts(const char* text);

// Read pending lines from the ring buffer into a flat buffer.
// Returns the number of new lines available since last read.
// `out` must point to maxLines * lineLen bytes.
uint32_t DebugOverlayRead(char* out, uint32_t maxLines, uint32_t lineLen);

// Get total number of lines written (may wrap).
uint32_t DebugOverlayTotalLines();

// Read pending lines using an external cursor (for independent consumers).
// Caller manages *cursor across calls. Returns number of lines read.
uint32_t DebugOverlayReadFrom(uint32_t* cursor, char* out, uint32_t maxLines, uint32_t lineLen);

// Spawn the kernel console WM window.
// Must be called after CompositorInit + WmInit + SchedulerInit.
void KernelConsoleSpawn();

// Spawn the TCP debug server (port 1234).
// Must be called after NetInit + SchedulerInit.
void DebugTcpSpawn();

} // namespace brook
