// syscall_cred.cpp -- credential syscalls: uid/gid, groups, capabilities.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "process_user_namespace.h"

#include "string.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_handlers.h"

namespace brook {

// ---------------------------------------------------------------------------
// Identity syscalls: getuid/getgid/geteuid/getegid/setuid/setgid
// ---------------------------------------------------------------------------

int64_t sys_getuid(uint64_t, uint64_t, uint64_t,
                    uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? ProcessReportId(proc,false,proc->uid) : 0;
}
int64_t sys_getgid(uint64_t, uint64_t, uint64_t,
                    uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? ProcessReportId(proc,true,proc->gid) : 0;
}
int64_t sys_geteuid(uint64_t, uint64_t, uint64_t,
                     uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? ProcessReportId(proc,false,proc->euid) : 0;
}
int64_t sys_getegid(uint64_t, uint64_t, uint64_t,
                     uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    return proc ? ProcessReportId(proc,true,proc->egid) : 0;
}
static int64_t ChangeIds(bool group, CredentialChange operation,
                         uint64_t real, uint64_t effective = CredentialUnchanged,
                         uint64_t saved = CredentialUnchanged) {
    auto* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    return ProcessChangeIds(proc, group, operation, static_cast<uint32_t>(real),
                            static_cast<uint32_t>(effective), static_cast<uint32_t>(saved));
}
int64_t sys_setuid(uint64_t id, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    return ChangeIds(false, CredentialChange::Single, id);
}
int64_t sys_setgid(uint64_t id, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    return ChangeIds(true, CredentialChange::Single, id);
}
int64_t sys_setreuid(uint64_t real, uint64_t effective, uint64_t, uint64_t, uint64_t, uint64_t) {
    return ChangeIds(false, CredentialChange::Re, real, effective);
}
int64_t sys_setregid(uint64_t real, uint64_t effective, uint64_t, uint64_t, uint64_t, uint64_t) {
    return ChangeIds(true, CredentialChange::Re, real, effective);
}
int64_t sys_setresuid(uint64_t real, uint64_t effective, uint64_t saved, uint64_t, uint64_t, uint64_t) {
    return ChangeIds(false, CredentialChange::Res, real, effective, saved);
}
int64_t sys_setresgid(uint64_t real, uint64_t effective, uint64_t saved, uint64_t, uint64_t, uint64_t) {
    return ChangeIds(true, CredentialChange::Res, real, effective, saved);
}

// Empty-capability API for tasks without retained root IDs. The current UID
// rules cannot introduce zero into an all-nonzero tuple, and exec grants no
// set-ID/file capabilities. Revisit this invariant before adding either feature.
// Root-capable tasks remain unsupported: do not pretend an empty capset drops
// their UID-based privileges. Bounding/ambient sets and securebits are separate.
static bool EmptyCapabilitiesSupported(Process* proc) {
    CredentialIds ids = ProcessReadIds(proc, false);
    return ids.real != 0 && ids.effective != 0 && ids.saved != 0;
}
static int CapabilityWords(uint64_t header, bool queryOnly) {
    uint32_t version;
    if (!CopyFromUser(&version, header, sizeof(version))) return -EFAULT;
    if (version == 0x19980330U) return 3; // V1: one 3-word structure.
    if (version == 0x20071026U || version == 0x20080522U) return 6; // V2/V3.
    version = 0x20080522U;
    if (!CopyToUser(header, &version, sizeof(version))) return -EFAULT;
    // Linux permits version discovery using capget(header, NULL).
    return queryOnly ? 0 : -EINVAL;
}
int64_t sys_capget(uint64_t header, uint64_t data, uint64_t,
                   uint64_t, uint64_t, uint64_t) {
    auto* proc = ProcessCurrent(); if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    UserCapabilityState capabilities{};bool privateCaps=ProcessReadCapabilities(proc,&capabilities)==0;
    if (!privateCaps && !EmptyCapabilitiesSupported(proc)) return -ENOSYS;
    int words = CapabilityWords(header, !data);
    if (words <= 0) return words;
    if (!data) return 0; // Valid-version null data does not require the PID field.
    int32_t pid;
    if (!CopyFromUser(&pid, header + sizeof(uint32_t), sizeof(pid))) return -EFAULT;
    if (pid < 0) return -EINVAL;
    if (pid && pid != proc->pid) return -ENOSYS; // Foreign queries not implemented.
    uint32_t values[6]={};
    if(privateCaps)for(unsigned i=0;i<unsigned(words/3);++i) {
        values[i*3]=capabilities.effective>>(i*32);values[i*3+1]=capabilities.permitted>>(i*32);values[i*3+2]=capabilities.inheritable>>(i*32);
    }
    return CopyToUser(data,values,words*sizeof(uint32_t))?0:-EFAULT;
}
int64_t sys_capset(uint64_t header, uint64_t data, uint64_t,
                   uint64_t, uint64_t, uint64_t) {
    auto* proc = ProcessCurrent(); if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    UserCapabilityState capabilities{};bool privateCaps=ProcessReadCapabilities(proc,&capabilities)==0;
    if (!privateCaps && !EmptyCapabilitiesSupported(proc)) return -ENOSYS;
    int words = CapabilityWords(header, false);
    if (words < 0) return words;
    int32_t pid;
    if (!CopyFromUser(&pid, header + sizeof(uint32_t), sizeof(pid))) return -EFAULT;
    if (pid && pid != proc->pid) return -EPERM;
    uint32_t values[6] = {};
    if (!CopyFromUser(values, data, words * sizeof(uint32_t))) return -EFAULT;
    if(privateCaps) {
        const uint64_t effective=values[0]|(uint64_t{values[3]}<<32),permitted=values[1]|(uint64_t{values[4]}<<32),inheritable=values[2]|(uint64_t{values[5]}<<32);
        return ProcessSetCapabilities(proc,effective,permitted,inheritable);
    }
    for (int i = 0; i < words; ++i) if (values[i]) return -EPERM;
    return 0; // An already-empty state remains empty.
}

int64_t sys_getgroups(uint64_t size, uint64_t listAddr, uint64_t,
                       uint64_t, uint64_t, uint64_t) {
    auto* proc = ProcessCurrent(); if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    struct Snapshot {
        SupplementaryGroups* groups;
        ~Snapshot() { ProcessGroupsRelease(groups); }
    } snapshot{ProcessGroupsAcquire(proc)};
    const uint32_t count = snapshot.groups ? snapshot.groups->count : 0;
    const int capacity = static_cast<int32_t>(size);
    if (capacity < 0 || (capacity && static_cast<uint32_t>(capacity) < count)) return -EINVAL;
    if (!capacity) return count;
    if (!UserBufferWritable(listAddr, uint64_t(count) * sizeof(uint32_t))) return -EFAULT;
    for(uint32_t i=0;i<count;++i) {
        const uint32_t id=ProcessReportId(proc,true,snapshot.groups->ids[i]);
        if(!CopyToUser(listAddr+uint64_t(i)*sizeof(id),&id,sizeof(id)))return -EFAULT;
    }
    return count;
}
static void SortGroupIds(uint32_t* ids, uint32_t count) {
    auto sift = [&](uint32_t root, uint32_t end) {
        while (root < end / 2) {
            uint32_t child = root * 2 + 1;
            if (child + 1 < end && ids[child] < ids[child + 1]) ++child;
            if (ids[root] >= ids[child]) break;
            uint32_t tmp = ids[root]; ids[root] = ids[child]; ids[child] = tmp;
            root = child;
        }
    };
    for (uint32_t start = count / 2; start; --start) sift(start - 1, count);
    for (uint32_t end = count; end > 1; --end) {
        uint32_t tmp = ids[0]; ids[0] = ids[end - 1]; ids[end - 1] = tmp;
        sift(0, end - 1);
    }
}
int64_t sys_setgroups(uint64_t size, uint64_t listAddr, uint64_t,
                       uint64_t, uint64_t, uint64_t) {
    auto* proc = ProcessCurrent(); if (!proc) return -ESRCH;
    KernelOperationGuard operation(proc);
    if (!ProcessSetgroupsAllowed(proc)) return -EPERM;
    const uint32_t count = static_cast<uint32_t>(size);
    if (count > MAX_SUPPLEMENTARY_GROUPS) return -EINVAL;
    if (!UserBufferReadable(listAddr, uint64_t(count) * sizeof(uint32_t))) return -EFAULT;
    SupplementaryGroups* groups = nullptr;
    if (count) {
        groups = static_cast<SupplementaryGroups*>(kmalloc(sizeof(SupplementaryGroups) + (count - 1) * sizeof(uint32_t)));
        if (!groups) return -ENOMEM;
        groups->references = 1; groups->count = count;
        if (!CopyFromUser(groups->ids, listAddr, count * sizeof(uint32_t))) { kfree(groups); return -EFAULT; }
        for (uint32_t i = 0; i < count; ++i) {
            groups->ids[i]=ProcessResolveId(proc,true,groups->ids[i]);
            if (groups->ids[i] == CredentialUnchanged) { kfree(groups); return -EINVAL; }
        }
        SortGroupIds(groups->ids, count); // keep duplicates, like Linux
    }
    ProcessGroupsReplace(proc, groups);
    return 0;
}

// ---------------------------------------------------------------------------
// sys_getresuid (118) / sys_getresgid (120)
// ---------------------------------------------------------------------------

static int64_t GetResIds(bool group, uint64_t real, uint64_t effective, uint64_t saved) {
    auto* proc = ProcessCurrent(); if (!proc) return -ESRCH;
    if (!UserBufferWritable(real, 4) || !UserBufferWritable(effective, 4) || !UserBufferWritable(saved, 4))
        return -EFAULT;
    const auto ids = ProcessReadIds(proc, group);
    const uint32_t values[3] = {ProcessReportId(proc,group,ids.real),
        ProcessReportId(proc,group,ids.effective),ProcessReportId(proc,group,ids.saved)};
    if (!CopyToUser(real, &values[0], 4) || !CopyToUser(effective, &values[1], 4) ||
        !CopyToUser(saved, &values[2], 4)) return -EFAULT;
    return 0;
}
int64_t sys_getresuid(uint64_t real, uint64_t effective, uint64_t saved, uint64_t, uint64_t, uint64_t) {
    return GetResIds(false, real, effective, saved);
}
int64_t sys_getresgid(uint64_t real, uint64_t effective, uint64_t saved, uint64_t, uint64_t, uint64_t) {
    return GetResIds(true, real, effective, saved);
}

} // namespace brook
