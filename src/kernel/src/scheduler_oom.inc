// Included inside namespace brook, sharing the production process registry.
static bool OomEligibleLocked(Process* anchor,AddressSpaceState* space,uint64_t incarnation) {
    if(!anchor || !space || space->kernel || anchor->pid<=1 || anchor->isKernelThread ||
       anchor->isThread || anchor->pid!=anchor->tgid || anchor->incarnation!=incarnation ||
       anchor->addressSpace!=space || anchor->state==ProcessState::Terminated ||
       anchor->tgidExiting || __atomic_load_n(&anchor->oomEpisode,__ATOMIC_ACQUIRE) || anchor->lifecycleRole==ProcessLifecycleRole::Service ||
       __atomic_load_n(&space->oomTerminating,__ATOMIC_ACQUIRE))return false;
    bool registered=false;
    for(uint32_t i=0;i<g_processCount;++i) {
        const auto* peer=g_allProcesses[i];if(peer==anchor)registered=true;
        if(peer->addressSpace!=space)continue;
        const auto group=peer->isThread?peer->leaderIncarnation:peer->incarnation;
        if(peer->isKernelThread || peer->lifecycleRole==ProcessLifecycleRole::Service ||
           peer->tgid!=anchor->tgid || group!=incarnation)return false;
    }
    return registered;
}
bool SchedulerNextOomCandidate(uint16_t after,Process** process,AddressSpaceState** space) {
    if(!process || !space)return false;
    *process=nullptr;*space=nullptr;
    for(;;) {
        const auto flags=SchedLockAcquire(g_allProcLock);
        Process* best=nullptr;
        for(uint32_t i=0;i<g_processCount;++i) {
            auto* candidate=g_allProcesses[i];
            if(candidate->pid<=after || (best && candidate->pid>=best->pid))continue;
            best=candidate;
        }
        if(!best){SchedLockRelease(g_allProcLock,flags);return false;}
        after=best->pid;
        const bool eligible=OomEligibleLocked(best,best->addressSpace,best->incarnation);
        if(eligible) {
            ProcessRef(best);AddressSpaceRef(best->addressSpace);
            *process=best;*space=best->addressSpace;
        }
        SchedLockRelease(g_allProcLock,flags);
        if(eligible)return true;
        // At most two registry scans per hold. Reacquire between rejected
        // candidates so a large protected population cannot mask IPIs.
    }
}
bool SchedulerOomCommitVictim(Process* process,AddressSpaceState* space,uint64_t incarnation) {
    // The common helper revalidates and latches under the same registry guard
    // as thread/CLONE_VM publication. It does not block for protected teardown.
    return RequestFatalSignalImpl(process,9,space,incarnation);
}
bool SchedulerOomGroupPresent(uint16_t tgid,uint64_t incarnation) {
    const auto flags=SchedLockAcquire(g_allProcLock);bool found=false;
    for(uint32_t i=0;i<g_processCount;++i) {
        const auto* process=g_allProcesses[i];
        const auto group=process->isThread?process->leaderIncarnation:process->incarnation;
        if(process->tgid==tgid && group==incarnation){found=true;break;}
    }
    SchedLockRelease(g_allProcLock,flags);return found;
}

bool SchedulerOomEpisodePresent(uint64_t episode) {
    if(!episode)return false;
    const auto flags=SchedLockAcquire(g_allProcLock);bool found=false;
    for(uint32_t i=0;i<g_processCount;++i)
        if(__atomic_load_n(&g_allProcesses[i]->oomEpisode,__ATOMIC_ACQUIRE)==episode){found=true;break;}
    SchedLockRelease(g_allProcLock,flags);return found;
}
