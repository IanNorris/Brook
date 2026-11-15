#pragma once
// Pipe poll-subscription types, shared by pipe_poll.inc and by poll/epoll waits.
// Included inside namespace brook after ProcessRef, ProcessUnref, kmalloc/kfree
// and the PipeBuffer helpers are declared (split from pipe_poll.inc).
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
