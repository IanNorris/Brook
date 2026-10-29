// Production scheduler lifecycle paths, included within namespace brook.
void SchedulerAddProcess(Process* proc)
{
    proc->state = ProcessState::Ready;

    // Kernel threads store fn/arg at kernelStackTop[-16] and [-8], so RSP
    // must start below those slots to avoid the function prologue overwriting them.
    proc->savedCtx.rsp = proc->isKernelThread
        ? proc->kernelStackTop - 24   // below fn/arg slots (16 bytes) + alignment
        : proc->kernelStackTop - 8;

    if (proc->isKernelThread)
        proc->savedCtx.rip = reinterpret_cast<uint64_t>(&KernelThreadTrampoline);
    else if (proc->isForkChild)
        proc->savedCtx.rip = reinterpret_cast<uint64_t>(&ForkChildTrampoline);
    else
        proc->savedCtx.rip = reinterpret_cast<uint64_t>(&ProcessTrampoline);

    proc->savedCtx.rflags = 0x202;
    proc->savedCtx.cr3 = proc->addressSpace->pageTable.pml4.raw();
    proc->savedCtx.fsBase = proc->fsBase;

    // Register with pid lookup and policy module atomically with birth below.
    // BRO-176/SIG2: g_pidToProcess[] and the policy state (RrState) are protected
    // by g_readyLock — PickNextLocked reads g_pidToProcess[pid] and the RR ops
    // mutate RrState all under it. These two writes were previously done WITHOUT
    // the lock, racing those locked readers/writers on other CPUs and corrupting
    // the intrusive ready-list (head/pid), which surfaced as a wild stack-region
    // pointer being scheduled as a Process*. Serialize them under g_readyLock.

    // BRO-173: serialize thread birth against group exit.  A new thread of an
    // already-exiting group must NOT become runnable — it was born after the
    // exit_group kill snapshot, so nothing would ever terminate it, the group
    // would never empty, and the leader/parent would livelock.  We check the
    // group leader's death-latch under g_allProcLock (the same lock
    // SchedulerKillThreadGroup sets it under), and if the group is exiting we
    // register the thread already-Terminated+reapable so it is reaped instead
    // of run.
    //
    // The check uses the thread's OWN generation-specific leader pointer, NOT a
    // scan for any process with a matching tgid: pids/tgids are reused, so a
    // tgid scan can false-positive against a still-unreaped corpse from a prior
    // generation and stillbirth a healthy new group's threads (which then hang
    // the new leader in pthread_create forever).  The leader pointer is stable
    // while it has live threads (it is parked in exit_group) and is validated
    // by magic; a dangling/invalid leader is treated as not-exiting (the normal
    // teardown paths still apply).
    bool stillborn = false;
    MountNamespace* retiredMounts=nullptr;
    // Match policy replacement's ready -> allProc order, and keep a newly
    // runnable thread from dispatching before its leader lifetime is counted.
    uint64_t birthFlags = SchedLockAcquire(g_readyLock);
    uint64_t alf1 = SchedLockAcquire(g_allProcLock);
    // A policy switch must see either the whole published birth or none of it.
    // Initializing before this lock allowed replacement to erase the newborn's
    // policy entry before it reached g_allProcesses and the ready queue.
    if (proc->pid < SCHED_MAX_PIDS)
        g_pidToProcess[proc->pid] = proc;
    g_schedOps->InitProcess(g_schedState, proc->pid, proc->schedPriority);
    g_runtime.Register(proc->pid, proc->incarnation, proc->tgid ? proc->tgid : proc->pid,
                       proc->isThread ? proc->leaderIncarnation : proc->incarnation);
    if (proc->isThread && !proc->isKernelThread)
    {
        Process* ldr = proc->threadLeader;
        if (ldr && ldr->magic == PROCESS_MAGIC && ldr->pid==proc->tgid &&
            ldr->incarnation==proc->leaderIncarnation)
        {
            // Reparenting may have happened after CopyParentProcess but
            // before publication. Refresh ancestry in the same registry lock.
            proc->parentPid=ldr->parentPid;
            if(__atomic_load_n(&ldr->tgidExiting, __ATOMIC_ACQUIRE) ||
               __atomic_load_n(&ldr->oomEpisode,__ATOMIC_ACQUIRE))stillborn = true;
        }
    }
    // An already-prepared cross-group CLONE_VM child must not escape the
    // selected AS after the fatal snapshot. Match OOM's registry lock and use
    // the existing stillborn resource cleanup for every user task.
    if(!proc->isKernelThread && (__atomic_load_n(&proc->oomEpisode,__ATOMIC_ACQUIRE) ||
       (proc->addressSpace && __atomic_load_n(&proc->addressSpace->oomTerminating,__ATOMIC_ACQUIRE))))stillborn=true;
    if (stillborn)
    {
        proc->state = ProcessState::Terminated;
        retiredMounts=ProcessFsRetireMounts(proc);
        proc->waitStatus = 0;
        __atomic_store_n(&proc->reapable, true, __ATOMIC_RELEASE);
        SerialPrintf("SCHED: thread pid=%u stillborn into exiting tgid=%u\n",
                     proc->pid, proc->tgid);
    }
    else
    {
        ReadyQueueInsertLocked(proc);
    }

    if (g_processCount < MAX_PROCESSES)
    {
        g_allProcesses[g_processCount++] = proc;
        ++g_totalForks;
        // BRO-176: register a thread against its leader's address space at the
        // exact moment it becomes visible in g_allProcesses (so it is guaranteed
        // to be reaped via ProcessDestroy, which decrements).  This keeps the
        // leader alive — and thus the shared page table / user pages mapped —
        // until every thread sharing the address space is gone, closing the
        // pick->switch UAF where a Terminated-but-pick-pending sibling resumes
        // on a freed cr3.  Balanced 1:1 with the decrement in ProcessDestroy.
        if (proc->isThread && !proc->isKernelThread)
        {
            Process* ldr = proc->threadLeader;
            if (ldr && ldr->magic == PROCESS_MAGIC)
            {
                int32_t after = __atomic_add_fetch(&ldr->asLiveThreads, 1, __ATOMIC_ACQ_REL);
                SchedulerRecordAsLive(ldr, proc->pid, ldr->pid, (uint32_t)ldr->incarnation,
                                      (uint32_t)proc->leaderIncarnation, after, /*op=inc*/0);
            }
        }
    }
    else
    {
        SchedLockRelease(g_allProcLock, alf1);
        KernelPanic("SCHED: g_allProcesses full (MAX_PROCESSES=%u). "
                    "Process '%s' pid=%u not registered.\n",
                    MAX_PROCESSES, proc->name, proc->pid);
    }
    // Publish before dispatch/removal can recycle this PID. Lock order is
    // ready -> registry -> PID; allocation/release never takes another lock
    // while holding PID. A shutdown fence must wait for these leases before
    // checking the registry for remaining userspace tasks.
    uint64_t pidFlags=SchedLockAcquire(g_pidLock);
    g_processIds.Publish(proc->pid);
    SchedLockRelease(g_pidLock,pidFlags);
    SchedLockRelease(g_allProcLock, alf1);
    SchedLockRelease(g_readyLock, birthFlags);
    ProcessFsFinishMountRetirement(retiredMounts);

    DbgPrintf("SCHED: added '%s' (pid %u) to ready queue\n",
                 proc->name, proc->pid);

    KickIdleCpu();
}
