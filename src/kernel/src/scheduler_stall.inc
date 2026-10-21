// Included by scheduler.cpp and the hosted actual-body lifetime regression.
// Caller is the idle BSP, after releasing its dispatch ready-lock acquisition.
static void ReportPersistentReadyStall(uint32_t cpu, uint32_t readyN)
{
    static uint64_t lastLog = 0;
    static uint16_t lastPid = SCHED_PID_NONE;
    static uint64_t lastIncarnation = 0;
    const uint64_t now = ApicTickCount();
    if (now - lastLog < 500) return;

    // Copy only values while the process registry and scheduler membership are
    // protected. A raw Process* cannot survive unlock: another CPU can kill,
    // detach and destroy it before even the next diagnostic read.
    struct Snapshot {
        uint16_t pid = SCHED_PID_NONE;
        uint64_t incarnation = 0;
        char name[32]{};
        uint64_t trace[12]{};
        brook::SchedDebugInfo policy{};
        bool persistent = false;
    } snapshot;
    const uint64_t rlf = SchedLockAcquire(g_readyLock);
    const uint64_t alf = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount; ++i)
    {
        Process* p = g_allProcesses[i];
        if (!p || p->magic != PROCESS_MAGIC || p->state != ProcessState::Ready ||
            !ProcessCanRunOnCpu(p, cpu) ||
            __atomic_load_n(&p->runningOnCpu, __ATOMIC_ACQUIRE) != -1)
            continue;
        // Ready-but-pending-requeue is the legitimate post-switch window, not
        // a lost enqueue. Read these scheduler-owned slots under readyLock.
        bool pending = false;
        for (uint32_t c = 0; c < SCHED_MAX_CPUS; ++c)
            if (g_perCpu[c].pendingRequeue == p) { pending = true; break; }
        if (pending) continue;
        snapshot.pid = p->pid;
        snapshot.incarnation = p->incarnation;
        snapshot.persistent = p->pid == lastPid && p->incarnation == lastIncarnation;
        if (snapshot.persistent)
        {
            for (unsigned k = 0; k + 1 < sizeof(snapshot.name); ++k)
                snapshot.name[k] = p->name[k];
            for (uint32_t k = 0; k < 12; ++k)
                snapshot.trace[k] = p->schedTrace[(p->schedTraceHead + k) % 12];
            if (g_schedOps->DebugDump)
                g_schedOps->DebugDump(g_schedState, p->pid, &snapshot.policy);
        }
        break;
    }
    lastPid = snapshot.pid;
    lastIncarnation = snapshot.incarnation;
    lastLog = now;
    SchedLockRelease(g_allProcLock, alf);
    SchedLockRelease(g_readyLock, rlf);
    if (!snapshot.persistent) return;

    // Printing is outside both locks and dereferences only the bounded copy.
    SerialPrintf("SCHED STALL: pid=%u '%s' state=Ready but PickNext "
                 "returned null (policy readyCount=%u) — persistent candidate\n",
                 snapshot.pid, snapshot.name, readyN);
    const auto& di = snapshot.policy;
    SerialPrintf("  POLICY[pid=%u]: queued=%u active=%u next=%u prev=%u | "
                 "head=%u tail=%u readyCount=%u listLen=%u%s\n",
                 snapshot.pid, di.queued, di.active, di.nextPid, di.prevPid,
                 di.head, di.tail, di.readyCount, di.listLen,
                 di.listLen != di.readyCount ? "  <<< STRUCT CORRUPT (listLen != readyCount)" :
                 di.queued ? "  <<< LOGIC DESYNC (queued but not picked)" :
                             "  <<< NOT ENQUEUED (queued=0)");
    SerialPrintf("  SCHEDTRACE pid=%u (oldest->newest):\n", snapshot.pid);
    for (uint32_t k = 0; k < 12; ++k)
    {
        const uint64_t e = snapshot.trace[k];
        if (!e) continue;
        SerialPrintf("    site=%u state=%u tick=%lu\n",
                     unsigned((e >> 8) & 0xFF), unsigned(e & 0xFF),
                     static_cast<unsigned long>(e >> 16));
    }
    SerialPrintf("  (sites 1=ENQ 2=REM 3=RDY_UNBLK 4=DEFER_RR 5=DEFER_CPU "
                 "6=BLOCK 7=BLOCK_SKIP 8=PREEMPT 9=YIELD 10=REQ_ENQ "
                 "11=REQ_SKIP 12=RUN 13=PICK_RET 14=PICK_SKIP_RUN "
                 "15=DS_BAIL_ENQ 16=DS_BAIL_SKIP 17=REM_PROC)\n");
}
