// Shared actual poll subscription implementation. Include after ProcessRef,
// ProcessUnref and WakeProcess definitions. Notification never allocates or
// retains a registration pointer after releasing the pipe lock.
struct PipePollNode {
    PipeBuffer* pipe;
    Process* process;
    uint32_t events;
    uint64_t sequence;
    PipePollNode* nextPipe;
    PipePollNode* nextGroup;
};

struct PipePollGroup {
    PipePollNode* nodes = nullptr;
    PipePollGroup() = default;
    PipePollGroup(const PipePollGroup&) = delete;
    PipePollGroup& operator=(const PipePollGroup&) = delete;
    bool add(PipeBuffer* pipe, Process* process, uint32_t events) {
        for (auto* n = nodes; n; n = n->nextGroup) {
            if (n->pipe != pipe) continue;
            SpinLockAcquire(&pipe->lock);
            n->events |= events;
            SpinLockRelease(&pipe->lock);
            return true;
        }
        auto* n = static_cast<PipePollNode*>(kmalloc(sizeof(PipePollNode)));
        if (!n) return false;
        ProcessRef(process);
        SpinLockAcquire(&pipe->lock);
        if (pipe->pollSequence == ~0ULL || pipe->pollHolds == ~0ULL) {
            SpinLockRelease(&pipe->lock);
            ProcessUnref(process);
            kfree(n);
            return false;
        }
        ++pipe->pollHolds;
        *n = {pipe, process, events, ++pipe->pollSequence, pipe->pollWaiters, nodes};
        pipe->pollWaiters = n;
        nodes = n;
        SpinLockRelease(&pipe->lock);
        return true;
    }
    ~PipePollGroup() {
        while (nodes) {
            auto* n = nodes;
            nodes = n->nextGroup;
            auto* pipe = n->pipe;
            SpinLockAcquire(&pipe->lock);
            auto** link = &pipe->pollWaiters;
            while (*link && *link != n) link = &(*link)->nextPipe;
            if (*link) *link = n->nextPipe;
            SpinLockRelease(&pipe->lock);
            ProcessUnref(n->process);
            kfree(n);
            PipeBufferDrop(pipe);
        }
    }
};

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
