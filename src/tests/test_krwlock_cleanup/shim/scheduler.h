#pragma once

// Host-test shim for scheduler.h (BRO-162).
// Declares only the scheduler entry points the real krwlock.cpp calls.
// The test (main.cpp) provides their implementations with a pthread-based
// mock that can model the "granted-but-not-yet-scheduled" window.

namespace brook {

struct Process;

void SchedulerBlock(Process* p);
void SchedulerUnblock(Process* p);

} // namespace brook
