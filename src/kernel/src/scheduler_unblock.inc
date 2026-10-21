// Production unblock/continue body shared with deterministic ownership tests.
static void SchedulerUnblockInternal(Process* proc, bool continued)
{
#ifdef BROOK_SCHED_BLOCK_HOST_TEST
    if (!proc) return;
#else
    uint64_t procAddr = reinterpret_cast<uint64_t>(proc);
    if (!proc || (procAddr >> 47) != 0x1FFFFULL)
        return;
#endif
    if (proc->magic != PROCESS_MAGIC)
        return;

    uint64_t rlf4 = SchedLockAcquire(g_readyLock);
    // Job-control resumption is separate from ordinary signal notification.
    // Decide under this lock, not via the sender's unlocked state snapshot.
    // An ignored/default SIGCONT must not wake an unrelated blocked syscall
    // or leave a wake latch on an already Running/Ready process.
    if (continued && proc->state != ProcessState::Stopped) {
        SchedLockRelease(g_readyLock, rlf4);
        return;
    }
    // Pipe/timer/lock wakeups are not SIGCONT. They must not silently resume
    // a job-control stop; terminal cancellation may resume an operation owner.
    if (proc->state == ProcessState::Stopped && !continued && !proc->terminalPending &&
        !__atomic_load_n(&proc->resumePending, __ATOMIC_ACQUIRE)) {
        SchedLockRelease(g_readyLock, rlf4);
        return;
    }
    // Accept Blocked or Stopped processes for unblocking/resuming
    if (proc->state != ProcessState::Blocked && proc->state != ProcessState::Stopped)
    {
        // If the process is still Running or Ready, it's in the window between
        // setting pollWaiter (inside the socket spinlock) and calling
        // SchedulerBlock.  Set pendingWakeup so the imminent SchedulerBlock
        // returns immediately instead of sleeping.
        //
        // Running: process on another CPU, about to call SchedulerBlock.
        // Ready:   process was preempted after releasing the socket spinlock
        //          but before reaching SchedulerBlock; when it next runs it
        //          will call SchedulerBlock and must not block.
        //
        // This mirrors the KMutexUnlock pattern which sets pendingWakeup
        // directly before calling SchedulerUnblock.
        if (proc->state == ProcessState::Running ||
            proc->state == ProcessState::Ready)
            __atomic_store_n(&proc->pendingWakeup, 1, __ATOMIC_RELEASE);
        SchedTrace(proc, STR_UNBLOCK_DEFER_RR);
        SchedLockRelease(g_readyLock, rlf4);
        return;
    }
    if (continued && proc->state == ProcessState::Stopped &&
        !__atomic_load_n(&proc->resumePending, __ATOMIC_ACQUIRE)) {
        proc->continuedPending = true;
        proc->stopReported = true;
        uint64_t alf = SchedLockAcquire(g_allProcLock);
        NotifyParentGroupLocked(proc);
        SchedLockRelease(g_allProcLock, alf);
    }
    if (__atomic_load_n(&proc->runningOnCpu, __ATOMIC_ACQUIRE) != -1)
    {
        // Never enqueue a stack still owned by a CPU. Keep stopped tasks
        // Stopped so their CPU completes the stop/switch-out path. A distinct
        // token survives parent consumption of the continued event and cannot
        // be confused with an ordinary I/O wakeup or SchedulerBlock fast path.
        if (proc->state == ProcessState::Stopped)
            __atomic_store_n(&proc->resumePending, true, __ATOMIC_RELEASE);
        else
            __atomic_store_n(&proc->pendingWakeup, 1, __ATOMIC_RELEASE);
        SchedTrace(proc, STR_UNBLOCK_DEFER_CPU);
        SchedLockRelease(g_readyLock, rlf4);
        return;
    }
    __atomic_store_n(&proc->resumePending, false, __ATOMIC_RELEASE);
    proc->state = ProcessState::Ready;
    const uint64_t wakeDeadline=proc->wakeupTick;
    proc->wakeupTick = 0;
    __atomic_store_n(&proc->pendingWakeup, 0, __ATOMIC_RELEASE);
    SchedTrace(proc, STR_READY_UNBLOCK);
    ReadyQueueInsertLocked(proc);
    if(wakeDeadline && g_readyLatency.Watching(proc->pid,proc->incarnation))
        g_readyLatency.Deadline(proc->pid,proc->incarnation,wakeDeadline,ApicTickCount());
    SchedLockRelease(g_readyLock, rlf4);

    KickIdleCpu();
}

void SchedulerUnblock(Process* proc) { SchedulerUnblockInternal(proc, false); }
void SchedulerContinue(Process* proc) { SchedulerUnblockInternal(proc, true); }
