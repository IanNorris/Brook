#pragma once
namespace brook {
struct Process;
// Current normal exit/exec, or a quiesced target retained by the cleanup queue.
// Uses the current executing worker as kernel-operation actor, not the target.
void RobustFutexCleanup(Process* target);
// IRQ-safe, after actual Terminated publication and outside scheduler locks.
// Takes one lifetime reference until cleanup and parent-notification completion.
void ProcessQueueRobustExit(Process* target);
}
