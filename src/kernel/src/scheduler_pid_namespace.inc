// The ready and registry locks serialize terminal publication with births.
// Drain requests run later, outside both locks, on the scheduler's timer path.
static uint32_t g_pidNamespaceDrains;

static void BeginPidNamespaceDrainLocked(const Process* last)
{
    if (!ProcessPidIsNamespaceInit(last)) return;
    if (PidNamespaceStopBirths(ProcessPidView(last)))
        __atomic_add_fetch(&g_pidNamespaceDrains, 1, __ATOMIC_RELEASE);
}

static bool PidNamespaceInitAwaitingDrain(const Process* process)
{
    return ProcessPidIsNamespaceInit(process) && PidNamespaceDraining(ProcessPidView(process));
}

static void DrainPidNamespaces()
{
    if (!__atomic_load_n(&g_pidNamespaceDrains, __ATOMIC_ACQUIRE)) return;
    Process* target = nullptr;
    const auto flags = SchedLockAcquire(g_allProcLock);
    for (uint32_t i = 0; i < g_processCount && !target; ++i) {
        Process* init = g_allProcesses[i];
        if (init->isThread || !PidNamespaceInitAwaitingDrain(init)) continue;
        const auto* view = ProcessPidView(init);
        bool hasDescendants = false;
        for (uint32_t j = 0; j < g_processCount; ++j) {
            Process* member = g_allProcesses[j];
            if (member->tgid == init->tgid || !ProcessVisibleTid(member, view)) continue;
            hasDescendants = true;
            // Catch exit-in-progress tasks too after their protected kernel
            // operation unwinds. The normal fatal path handles group-wide
            // quiescence and descriptor, mount and robust-list cleanup.
            if (member->state != ProcessState::Terminated && !member->terminalPending) {
                target = member;
                ProcessRef(target);
                break;
            }
        }
        // The reaper unlinks a task before the cleanup worker releases its
        // descriptors. Keep PID 1 un-waitable until those leases are retired.
        if (!hasDescendants && PidNamespaceTaskCount(view) == 1) {
            PidNamespaceFinishDrain(view);
            __atomic_sub_fetch(&g_pidNamespaceDrains, 1, __ATOMIC_RELEASE);
            NotifyExitedParentLocked(init);
        }
    }
    SchedLockRelease(g_allProcLock, flags);
    if (target) {
        SchedulerRequestFatalSignal(target, 9);
        ReleaseSchedulerReference(target, false);
    }
}
