// Shared actual poll subscription implementation. Include after ProcessRef,
// ProcessUnref and WakeProcess definitions. Notification never allocates or
// retains a registration pointer after releasing the pipe lock.
#include "pipe_poll_types.inc"

void PipePollNotify(PipeBuffer* pipe, uint32_t events) {
    // The caller owns live storage at entry. Keep our own hold across every
    // unlocked wake, including destruction of the final subscription.
    PipeBufferHold(pipe);
    // Snapshot the newest registration. New nodes are prepended with strictly
    // increasing identities and perform a readiness recheck after registering.
    // Walk older identities, taking only a process reference across unlock.
    // Concurrent unregister/free and overlapping notifications are independent.
    SpinLockAcquire(&pipe->lock);
    uint64_t cursor = pipe->pollSequence;
    SpinLockRelease(&pipe->lock);
    while (cursor) {
        Process* process = nullptr;
        SpinLockAcquire(&pipe->lock);
        for (auto* n = pipe->pollWaiters; n; n = n->nextPipe) {
            if (n->sequence > cursor || !(n->events & events)) continue;
            cursor = n->sequence - 1;
            process = n->process;
            ProcessRef(process);
            break;
        }
        SpinLockRelease(&pipe->lock);
        if (!process) break;
        WakeProcess(process);
        ProcessUnref(process);
    }
    PipeBufferDrop(pipe);
}
