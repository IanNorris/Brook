#pragma once

namespace brook {

// Start the software watchdog kernel thread.
// Monitors per-CPU tick counters; if ALL CPUs stop scheduling for
// WATCHDOG_TIMEOUT_MS (default 3000), forces a kernel panic with
// diagnostic info.
void WatchdogInit();

} // namespace brook
