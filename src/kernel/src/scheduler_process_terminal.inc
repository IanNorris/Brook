// Production scheduler lifecycle paths, included within namespace brook.
bool SchedulerKernelOperationBoundary()
{
    Process* proc = SchedulerCurrentProcess();
    if (!proc) return false;
    uint64_t flags = SchedLockAcquire(g_readyLock);
    if (proc->kernelRegionDepth) {
        SchedLockRelease(g_readyLock, flags);
        return false;
    }
    proc->kernelOperationProtected = false;
    bool pending = proc->terminalPending;
    int status = proc->pendingWaitStatus;
    proc->terminalPending = false;
    // Keep IF disabled until terminal publication: a killed current thread
    // must not leave a temporary reference behind on a discarded kernel stack.
    SchedLockReleaseRaw(g_readyLock);
    if (pending) {
        SchedulerMarkTerminated(proc, status);
        __atomic_store_n(&proc->reapable, true, __ATOMIC_RELEASE);
    }
    bool terminal = proc->state == ProcessState::Terminated;
    if (flags & 0x200) __asm__ volatile("sti" ::: "memory");
    return terminal;
}

void SchedulerMarkTerminated(Process* proc, int waitStatus)
{
    MountNamespace* retiredMounts=nullptr;
    // Hold across off-CPU waiter cancellation, including already-reapable
    // targets. All nonterminal state transitions use this same ready lock.
    uint64_t rlf = SchedLockAcquire(g_readyLock);
    ProcessRef(proc);
    if (proc->state != ProcessState::Terminated &&
        (proc->kernelRegionDepth || proc->kernelOperationProtected)) {
        if (!proc->terminalPending) proc->pendingWaitStatus = waitStatus;
        proc->terminalPending = true;
        bool resume = proc->state == ProcessState::Stopped || proc->interruptibleWait;
        SchedLockReleaseRaw(g_readyLock);
        // A stopped owner or explicitly interruptible waiter must unwind.
        // Ordinary blocked mutex waiters still wait for an ownership grant.
        if (resume) SchedulerUnblock(proc);
        ProcessUnref(proc);
        if (rlf & 0x200) __asm__ volatile("sti" ::: "memory");
        return;
    }
    if (proc->state != ProcessState::Terminated)
    {
        retiredMounts=ProcessFsRetireMounts(proc);
        proc->waitStatus = waitStatus;
        __atomic_store_n(reinterpret_cast<uint8_t*>(&proc->state),
                         static_cast<uint8_t>(ProcessState::Terminated),
                         __ATOMIC_RELEASE);
    }
    ReadyQueueRemoveLocked(proc);
    uint64_t parentFlags = SchedLockAcquire(g_allProcLock);
    ReparentDeadGroupLocked(proc->tgid);
    SchedLockRelease(g_allProcLock, parentFlags);
    bool offCpu = __atomic_load_n(&proc->runningOnCpu, __ATOMIC_ACQUIRE) == -1;
    // In a self-fatal path, restoring IF before dropping our temporary hold
    // could retire this CPU forever with the hold still on its dead stack.
    SchedLockReleaseRaw(g_readyLock);
    ProcessQueueRobustExit(proc); // queue ref gates successful wait/reap
    ProcessFsFinishMountRetirement(retiredMounts);
    PosixTimersCleanup(proc, false);
    SysvSemExit(proc);
    if (offCpu) FutexCancelProcess(proc);
    ProcessUnref(proc);
    if (rlf & 0x200) __asm__ volatile("sti" ::: "memory");
}

