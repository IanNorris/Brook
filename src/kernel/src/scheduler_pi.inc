// Production PI dispatch overlay; included within namespace brook.
struct PiDispatchRank { uint64_t incarnation; uint8_t rank; };
static PiDispatchRank g_piDispatch[SCHED_MAX_PIDS];
static unsigned g_piDispatchCount;
static uint16_t g_piDispatchCursor;

void SchedulerSetPiDonation(Process* proc, uint8_t rank)
{
    if (!proc || proc->pid >= SCHED_MAX_PIDS) return;
    const auto flags = SchedLockAcquire(g_readyLock);
    auto& entry = g_piDispatch[proc->pid];
    if (entry.incarnation) --g_piDispatchCount;
    entry = {};
    if (rank < proc->schedPriority && rank < 4 && proc->state != ProcessState::Terminated) {
        entry = {proc->incarnation, rank};
        ++g_piDispatchCount;
    }
    SchedLockRelease(g_readyLock, flags);
}

static uint8_t PiEffectiveRankLocked(const Process* proc)
{
    const auto& entry = g_piDispatch[proc->pid];
    return entry.incarnation == proc->incarnation && entry.rank < proc->schedPriority
        ? entry.rank : proc->schedPriority;
}

// Called only with readyLock held. An inherited rank overrides fair-policy
// selection while the donation is live; policy service accounting is retained.
// Equal-rank owners rotate by PID rather than starving one another forever.
static uint16_t PiCandidateLocked(uint32_t cpu)
{
    if (!g_piDispatchCount) return SCHED_PID_NONE;
    uint16_t best = SCHED_PID_NONE;
    uint8_t rank = 4;
    for (unsigned i = 1; i <= SCHED_MAX_PIDS; ++i) {
        const auto pid = static_cast<uint16_t>((g_piDispatchCursor + i) % SCHED_MAX_PIDS);
        auto* proc = g_pidToProcess[pid];
        if (!proc || proc->state != ProcessState::Ready ||
            __atomic_load_n(&proc->runningOnCpu, __ATOMIC_ACQUIRE) != -1 || !ProcessCanRunOnCpu(proc, cpu)) continue;
#ifdef BROOK_SCHED_START_TEST_HOOKS
        if (!StartTestEligible(proc, cpu)) continue;
#endif
        const auto effective = PiEffectiveRankLocked(proc);
        if (effective >= rank) continue;
        best = pid;
        rank = effective;
    }
    return best;
}

