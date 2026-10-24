// One commit: runtime accounting, old AS membership, group identity and pending
// child signals. Parent notification cannot interleave through the registry lock.
void SchedulerCommitExecThreadIdentity(Process* proc)
{
    if(!proc)return;
    const uint64_t flags=SchedLockAcquire(g_readyLock);
    const uint32_t cpu=ThisCpu();
    RuntimeChargeLocked(cpu,RuntimeNow(cpu));
    const uint64_t allFlags=SchedLockAcquire(g_allProcLock);
    TransferChildSubreaperExecLocked(proc);
    ReleaseThreadAddressSpaceLocked(proc);
    ProcessCommitExecIdentity(proc);
    g_runtime.Rebind(proc->pid,proc->incarnation,proc->tgid,proc->incarnation);
    SchedLockRelease(g_allProcLock,allFlags);
    SchedLockRelease(g_readyLock,flags);
}
