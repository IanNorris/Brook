// Thread-group subreaper state. Registry membership and all records below are
// protected by g_allProcLock. Incarnations prevent PID reuse from inheriting it;
// a fork's new identity starts clear, threads share it, exec preserves it.
struct ChildSubreaperRecord { uint64_t incarnation;bool enabled; };
static ChildSubreaperRecord g_childSubreapers[MAX_PROCESSES]{};
static uint64_t SubreaperGroupIncarnation(const Process* proc) {
    return proc->isThread?proc->leaderIncarnation:proc->incarnation;
}
static bool ChildSubreaperEnabledLocked(const Process* proc) {
    if(proc->tgid>=MAX_PROCESSES)return false;
    const auto& record=g_childSubreapers[proc->tgid];
    return record.incarnation==SubreaperGroupIncarnation(proc) && record.enabled;
}
int SchedulerChildSubreaperControl(Process* proc,bool set,bool enabled) {
    if(!proc)return -3; // ESRCH
    const uint64_t flags=SchedLockAcquire(g_allProcLock);
    int result=0;
    if(proc->tgid>=MAX_PROCESSES || proc->state==ProcessState::Terminated)result=-3;
    else if(set)g_childSubreapers[proc->tgid]={SubreaperGroupIncarnation(proc),enabled};
    else result=ChildSubreaperEnabledLocked(proc)?1:0;
    SchedLockRelease(g_allProcLock,flags);
    return result;
}
static uint16_t FindChildSubreaperLocked(uint16_t dyingGroup) {
    uint16_t group=dyingGroup;
    // Walk while the dying group's retained registry identity still exists.
    // No escaping pointers, allocations or cached ancestry invalidation needed.
    for(uint32_t depth=0;group && depth<MAX_PROCESSES;++depth) {
        Process* representative=nullptr;
        for(uint32_t i=0;i<g_processCount;++i) {
            Process* p=g_allProcesses[i];
            if(p->pid==group && p->tgid==group && !p->isThread)representative=p;
        }
        bool live=false;
        if(representative)for(uint32_t i=0;i<g_processCount;++i) {
            Process* p=g_allProcesses[i];
            if(p->tgid==group && SubreaperGroupIncarnation(p)==representative->incarnation &&
               p->state!=ProcessState::Terminated && !p->terminalPending)live=true;
        }
        // tgidExiting is also an exec-time thread-birth latch. It must not
        // hide the surviving subreaper while exec quiesces its siblings.
        if(group!=dyingGroup && live && ChildSubreaperEnabledLocked(representative))return group;
        if(!representative || representative->parentPid==group)return 0;
        group=representative->parentPid;
    }
    return 0; // Brook's existing kernel-init/reap fallback
}
// Called under ready -> registry locks, before a nonleader changes its TGID.
static void TransferChildSubreaperExecLocked(Process* proc) {
    const uint16_t oldGroup=proc->tgid;
    const uint64_t oldIncarnation=SubreaperGroupIncarnation(proc);
    const bool enabled=ChildSubreaperEnabledLocked(proc);
    if(proc->pid<MAX_PROCESSES)g_childSubreapers[proc->pid]={proc->incarnation,enabled};
    if(oldGroup!=proc->pid) {
        // Brook currently detaches nonleader exec instead of Linux de_thread.
        // Do not steal children or clear the setting of a still-live old group.
        for(uint32_t i=0;i<g_processCount;++i) {
            Process* peer=g_allProcesses[i];
            if(peer!=proc && peer->tgid==oldGroup &&
               SubreaperGroupIncarnation(peer)==oldIncarnation &&
               peer->state!=ProcessState::Terminated)return;
        }
        for(uint32_t i=0;i<g_processCount;++i)
            if(g_allProcesses[i]->parentPid==oldGroup)g_allProcesses[i]->parentPid=proc->pid;
        if(oldGroup<MAX_PROCESSES && g_childSubreapers[oldGroup].incarnation==oldIncarnation)
            g_childSubreapers[oldGroup]={};
    }
}
