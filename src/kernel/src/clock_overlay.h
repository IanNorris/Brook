#pragma once

namespace brook {

// Start the clock overlay kernel thread. Renders HH:MM:SS uptime in the
// top-right corner of the physical framebuffer, updating every second.
// Must be called after CompositorInit() and SchedulerInit().
void ClockOverlayStart();

} // namespace brook
