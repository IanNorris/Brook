static SpinLock g_credentialsLock;
#include "process_user_namespace.inc"
CredentialIds ProcessReadIds(Process* proc, bool group) {
    ProcessOperationGuard credentialOperation(ProcessCurrent());
    SpinLockAcquire(&g_credentialsLock);
    CredentialIds ids = group ? CredentialIds{proc->gid, proc->egid, proc->sgid}
                              : CredentialIds{proc->uid, proc->euid, proc->suid};
    SpinLockRelease(&g_credentialsLock); return ids;
}
int ProcessChangeIds(Process* proc, bool group, CredentialChange operation,
                     uint32_t real, uint32_t effective, uint32_t saved) {
    ProcessOperationGuard credentialOperation(ProcessCurrent());
    SpinLockAcquire(&g_credentialsLock);
    CredentialIds ids = group ? CredentialIds{proc->gid, proc->egid, proc->sgid}
                              : CredentialIds{proc->uid, proc->euid, proc->suid};
    const auto oldUsers=CredentialIds{proc->uid,proc->euid,proc->suid};
    auto translate=[&](uint32_t& id) {
        if(id==CredentialUnchanged)return operation!=CredentialChange::Single || &id!=&real;
        id=UserIdToCanonical(proc->userNamespace,group,id);return id!=UserIdInvalid;
    };
    if(!translate(real) || !translate(effective) || !translate(saved)) {SpinLockRelease(&g_credentialsLock);return -22;}
    int error = ChangeCredentialIds(ids, operation, UserNamespaceCapable(UserAuthorityLocked(proc),proc->userNamespace,group?CapSetgid:CapSetuid), real, effective, saved);
    if (!error) {
        if (group) { proc->gid = ids.real; proc->egid = ids.effective; proc->sgid = ids.saved; }
        else { proc->uid = ids.real; proc->euid = ids.effective; proc->suid = ids.saved; }
    }
    if(!error && !group && proc->userNamespace) {
        const bool wasRoot=UserIdFromCanonical(proc->userNamespace,false,oldUsers.real)==0 ||
            UserIdFromCanonical(proc->userNamespace,false,oldUsers.effective)==0 || UserIdFromCanonical(proc->userNamespace,false,oldUsers.saved)==0;
        const bool nowRoot=UserIdFromCanonical(proc->userNamespace,false,proc->uid)==0 ||
            UserIdFromCanonical(proc->userNamespace,false,proc->euid)==0 || UserIdFromCanonical(proc->userNamespace,false,proc->suid)==0;
        if(wasRoot && !nowRoot)proc->capabilities.permitted=proc->capabilities.effective=0;
        else if(UserIdFromCanonical(proc->userNamespace,false,oldUsers.effective)==0 && UserIdFromCanonical(proc->userNamespace,false,proc->euid)!=0)proc->capabilities.effective=0;
        else if(UserIdFromCanonical(proc->userNamespace,false,oldUsers.effective)!=0 && UserIdFromCanonical(proc->userNamespace,false,proc->euid)==0)proc->capabilities.effective=proc->capabilities.permitted;
    }
    SpinLockRelease(&g_credentialsLock); return error;
}
SupplementaryGroups* ProcessGroupsAcquire(Process* proc) {
    ProcessOperationGuard credentialOperation(ProcessCurrent());
    SpinLockAcquire(&g_credentialsLock);
    auto* groups = proc->groups;
    if (groups) __atomic_fetch_add(&groups->references, 1, __ATOMIC_RELAXED);
    SpinLockRelease(&g_credentialsLock); return groups;
}
void ProcessGroupsRelease(SupplementaryGroups* groups) {
    if (groups && __atomic_fetch_sub(&groups->references, 1, __ATOMIC_ACQ_REL) == 1) kfree(groups);
}
void ProcessGroupsReplace(Process* proc, SupplementaryGroups* owned) {
    ProcessOperationGuard credentialOperation(ProcessCurrent());
    SpinLockAcquire(&g_credentialsLock);
    auto* old = proc->groups; proc->groups = owned;
    SpinLockRelease(&g_credentialsLock); ProcessGroupsRelease(old);
}
FileCredentialAccess ProcessFileAccess(Process* proc, uint32_t owner, uint32_t group, uint16_t mode, bool realIds) {
    if (!proc) return {true, false, false, 7};
    ProcessOperationGuard credentialOperation(ProcessCurrent());
    SpinLockAcquire(&g_credentialsLock);
    uint32_t uid = realIds ? proc->uid : proc->euid;
    uint32_t gid = realIds ? proc->gid : proc->egid;
    FileCredentialAccess access = {uid == 0 && !proc->userNamespace, uid == owner, gid == group, 0};
    auto* groups = proc->groups;
    // Published group lists are sorted and immutable. Membership is needed
    // even for the owner (chmod's setgid rule), unlike the access-bit choice.
    uint32_t low = 0, high = groups ? groups->count : 0;
    while (!access.group && low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (groups->ids[mid] == group) access.group = true;
        else if (groups->ids[mid] < group) low = mid + 1;
        else high = mid;
    }
    access.permissions = access.privileged ? 7 : access.owner ? (mode >> 6) & 7
                         : access.group ? (mode >> 3) & 7 : mode & 7;
    SpinLockRelease(&g_credentialsLock);
    return access;
}
FileActorSnapshot ProcessFileActorAcquire(Process* proc,UserNamespace* filesystemOwner,bool realIds) {
    FileActorSnapshot result{};
    if(!proc)return result;
    ProcessOperationGuard operation(ProcessCurrent());
    SpinLockAcquire(&g_credentialsLock);
    result.uid=realIds?proc->uid:proc->euid;result.gid=realIds?proc->gid:proc->egid;
    result.initialNamespace=!proc->userNamespace;
    if(proc->userNamespace) {
        const auto& users=proc->userNamespace->users;const auto& groups=proc->userNamespace->groups;
        if(users.written)result.mappedUid=users.canonical;
        if(groups.written)result.mappedGid=groups.canonical;
    }
    auto actor=UserAuthorityLocked(proc);
    const unsigned wanted[]={CapChown,CapDacOverride,CapFowner,CapFsetid};
    for(unsigned cap:wanted)
        if(UserNamespaceCapable(actor,filesystemOwner,cap))result.capabilities|=uint64_t{1}<<cap;
    if(realIds) {
        // Preserve access(real-ID) policy: non-root uses permission bits, not
        // effective capabilities. Initial real root retains its DAC override.
        if(!result.initialNamespace || result.uid!=0)result.capabilities=0;
        else result.capabilities|=uint64_t{1}<<CapDacOverride;
    }
    result.groups=proc->groups;
    if(result.groups)__atomic_fetch_add(&result.groups->references,1,__ATOMIC_RELAXED);
    SpinLockRelease(&g_credentialsLock);return result;
}
int ProcessCheckFileAccess(Process* proc, uint32_t owner, uint32_t group, uint16_t mode,
                           uint8_t required, bool realIds) {
    if (required & ~7u) return -22;
    auto access = ProcessFileAccess(proc, owner, group, mode, realIds);
    // Linux root access(X_OK) still requires an execute bit on regular files.
    // Directory search uses the root override even when its mode is 000.
    if (access.privileged && (required & 1) && (mode & 0170000) == 0100000 && !(mode & 0111)) return -13;
    return (access.permissions & required) == required ? 0 : -13;
}
uint8_t ProcessFilePermissions(Process* proc, uint32_t owner, uint32_t group, uint16_t mode) {
    return ProcessFileAccess(proc, owner, group, mode).permissions;
}
// Exec currently grants neither set-ID nor file capabilities. Any future exec
// credential-grant path MUST honor this irreversible per-task restriction.
void ProcessSetNoNewPrivs(Process* proc) {
    if (!proc) return;
    ProcessOperationGuard operation(ProcessCurrent());
    SpinLockAcquire(&g_credentialsLock);
    proc->noNewPrivs = 1;
    SpinLockRelease(&g_credentialsLock);
}
bool ProcessGetNoNewPrivs(Process* proc) {
    if (!proc) return false;
    ProcessOperationGuard operation(ProcessCurrent());
    SpinLockAcquire(&g_credentialsLock);
    bool value = proc->noNewPrivs != 0;
    SpinLockRelease(&g_credentialsLock);
    return value;
}

static void InheritCredentials(Process* child, Process* parent) {
    ProcessOperationGuard credentialOperation(ProcessCurrent());
    SpinLockAcquire(&g_credentialsLock);
    child->uid = parent->uid; child->euid = parent->euid; child->suid = parent->suid;
    child->gid = parent->gid; child->egid = parent->egid; child->sgid = parent->sgid;
    child->noNewPrivs = parent->noNewPrivs;
    child->userNamespace=parent->userNamespace;UserNamespaceRetain(child->userNamespace);
    child->capabilities=parent->capabilities;
    child->groups = parent->groups;
    if (child->groups) __atomic_fetch_add(&child->groups->references, 1, __ATOMIC_RELAXED);
    SpinLockRelease(&g_credentialsLock);
}
