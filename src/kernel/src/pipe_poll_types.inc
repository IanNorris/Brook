#pragma once
// Pipe poll-subscription types, shared by pipe_poll.inc and by poll/epoll waits.
// Included inside namespace brook after ProcessRef, ProcessUnref, kmalloc/kfree
// and the PipeBuffer helpers are declared (split from pipe_poll.inc).
static constexpr uint32_t PipePollNodeLive = 0x4c4c4f50u; // "POLL"
static constexpr uint32_t PipePollNodeDead = 0x44414544u; // "DEAD"
struct PipePollNode {
    uint32_t magic;
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
    // A node reached through a pipe's list or this group must be live; a
    // stale one traps here (named by RIP) before it is dereferenced further.
    static void Check(const PipePollNode* n) {
        if (__atomic_load_n(&n->magic, __ATOMIC_RELAXED) != PipePollNodeLive) __builtin_trap();
    }
    bool add(PipeBuffer* pipe, Process* process, uint32_t events) {
        PipeBufferCheck(pipe);
        for (auto* n = nodes; n; n = n->nextGroup) {
            Check(n);
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
        *n = {PipePollNodeLive, pipe, process, events, ++pipe->pollSequence, pipe->pollWaiters, nodes};
        pipe->pollWaiters = n;
        nodes = n;
        SpinLockRelease(&pipe->lock);
        return true;
    }
    ~PipePollGroup() {
        while (nodes) {
            auto* n = nodes;
            Check(n);
            nodes = n->nextGroup;
            auto* pipe = n->pipe;
            PipeBufferCheck(pipe);
            SpinLockAcquire(&pipe->lock);
            auto** link = &pipe->pollWaiters;
            while (*link && *link != n) { Check(*link); link = &(*link)->nextPipe; }
            // Every subscription stays linked until its own group removes it.
            if (!*link) __builtin_trap();
            *link = n->nextPipe;
            n->magic = PipePollNodeDead;
            SpinLockRelease(&pipe->lock);
            ProcessUnref(n->process);
            kfree(n);
            PipeBufferDrop(pipe);
        }
    }
};
