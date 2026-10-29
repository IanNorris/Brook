// Production fatal-request implementation, also compiled by hosted race tests.
static bool RequestFatalSignalImpl(Process* anchor,int signal,AddressSpaceState* expectedSpace,uint64_t expectedIncarnation)
{
    // Signal senders include the timer ISR and TerminalClose with its mutex
    // held. Never wait for peers or enable IRQs while snapshot refs are live:
    // the target group can include this very CPU's interrupted/current task.
    uint64_t flags=0;
#ifndef BROOK_HOST_TEST
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
#endif
    Process* targets[MAX_PROCESSES];
    uint32_t count = 0;
    const uint64_t alf = SchedLockAcquire(g_allProcLock);
    if(expectedSpace && !OomEligibleLocked(anchor,expectedSpace,expectedIncarnation)) {
        SchedLockRelease(g_allProcLock,alf);
#ifndef BROOK_HOST_TEST
        if(flags&0x200)__asm__ volatile("sti":::"memory");
#endif
        return false;
    }
    if(expectedSpace)__atomic_store_n(&expectedSpace->oomTerminating,true,__ATOMIC_RELEASE);
    const uint16_t tgid = anchor->tgid;
    const uint64_t generation = anchor->isThread ? anchor->leaderIncarnation : anchor->incarnation;
    for (uint32_t i = 0; i < g_processCount; ++i) {
        Process* p = g_allProcesses[i];
        const uint64_t identity = p->isThread ? p->leaderIncarnation : p->incarnation;
        if (p->tgid != tgid || identity != generation) continue;
        // Same lock as thread publication. A racing clone is either included
        // here or sees its generation-specific leader's exiting latch.
        if(expectedSpace)__atomic_store_n(&p->oomEpisode,expectedSpace->identity,__ATOMIC_RELEASE);
        __atomic_store_n(&p->tgidExiting, true, __ATOMIC_RELEASE);
        ProcessRef(p);
        targets[count++] = p;
    }
    SchedLockRelease(g_allProcLock, alf);
    PosixTimersCleanup(anchor, true);
    for (uint32_t i = 0; i < count; ++i) {
        SchedulerMarkTerminated(targets[i], signal & 0x7f);
        __atomic_store_n(&targets[i]->reapable, true, __ATOMIC_RELEASE);
    }
    // Waiters with protected kernel operations are resumed to unwind. Off-CPU
    // terminal futex waiters are canceled by this notification-safe ref drop.
    for (uint32_t i = 0; i < count; ++i)
        ReleaseSchedulerReference(targets[i], false);
#ifndef BROOK_HOST_TEST
    if (flags & 0x200) __asm__ volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
    return true;
}

void SchedulerRequestFatalSignal(Process* anchor,int signal) {
    (void)RequestFatalSignalImpl(anchor,signal,nullptr,0);
}
