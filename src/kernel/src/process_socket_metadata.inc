// Included after g_credentialsLock and UserAuthorityLocked. The target has an
// owned Process reference. Export only socket scalars, not a reopen capability.
int ProcessReadSocketMetadata(Process* viewer,Process* target,uint32_t fd,SocketIdentity* out)
{
    if(!viewer || !target || !out)return -13;
    ProcessOperationGuard operation(ProcessCurrent());
    for(;;) {
        SpinLockAcquire(&g_credentialsLock);
        auto* fdLock=ProcessFdLock(target);
        // Never wait for an FD owner with credentials locked: that owner may
        // need the credential lock. Retry both snapshots after dropping it.
        if(!SpinLockTryAcquire(fdLock)) {
            SpinLockRelease(&g_credentialsLock);SchedulerYield();continue;
        }
        const bool sameGroup=viewer->tgid==target->tgid &&
            (viewer->isThread?viewer->leaderIncarnation:viewer->incarnation)==
            (target->isThread?target->leaderIncarnation:target->incarnation);
        const bool sameOwner=viewer->userNamespace==target->userNamespace &&
            viewer->euid==target->uid && viewer->euid==target->euid && viewer->euid==target->suid &&
            viewer->egid==target->gid && viewer->egid==target->egid && viewer->egid==target->sgid &&
            !(target->capabilities.permitted & ~viewer->capabilities.effective);
        const bool allowed=sameGroup || sameOwner ||
            UserNamespaceCapable(UserAuthorityLocked(viewer),target->userNamespace,19); // CAP_SYS_PTRACE
        int error=-2;
        if(target->fds && fd<MAX_FDS) {
            const auto& entry=target->fds[fd];
            if(entry.type!=FdType::None && !entry.closing) {
                error=0; // non-sockets retain the existing, separate policy
                if((entry.type==FdType::Socket || entry.type==FdType::NetlinkSocket) && entry.handle) {
                    error=!allowed?-13:
                        entry.type==FdType::NetlinkSocket?(NetlinkGetIdentity(entry.handle,out)?1:-2):
                        SockGetIdentity(static_cast<int>(reinterpret_cast<uintptr_t>(entry.handle))-1,out)?1:-2;
                }
            }
        }
        SpinLockRelease(fdLock); // release exactly the lock acquired
        SpinLockRelease(&g_credentialsLock);return error;
    }
}
