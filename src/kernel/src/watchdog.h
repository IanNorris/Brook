#pragma once

namespace brook {

// Enqueue the software monitor thread. It checks per-CPU timer accounting,
// but depends on the scheduler and timer to run: it cannot reliably detect a
// system-wide hard lockup or all causes of an unresponsive desktop. A truly
// independent watchdog is still required for those cases.
void WatchdogInit();

} // namespace brook
