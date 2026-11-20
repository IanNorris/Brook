#include "procfs_vfs.h"
#include "vfs.h"
#include "mount_namespace.h"
#include "procfs.h"
#include "memory/heap.h"
#include "pid_namespace.h"
#include "process.h"
#include "process_user_namespace.h"

namespace brook {

static bool ProcFsMount(uint8_t /*pdrv*/, void** mountPriv)
{
    *mountPriv = nullptr;
    return true;
}

static int ProcFsUnmountCb(void* mountPriv)
{
    auto* instance = static_cast<ProcMountView*>(mountPriv);
    if (instance) {
        PidNamespaceRelease(instance->pid);
        kfree(instance);
    }
    return 0;
}

static Vnode* ProcFsOpenCb(void* mountPriv, uint8_t /*pdrv*/,
                           const char* relPath, int flags)
{
    // Private proc mounts resolve through retained objects. The legacy path
    // callbacks have no namespace context and must not expose the host view.
    if (mountPriv) return nullptr;
    if (!relPath || !relPath[0] || (relPath[0] == '/' && !relPath[1]))
        return ProcFsOpenDir(relPath);
    const char* p = relPath;
    if (p[0] == '/') p++;
    return ProcFsOpen(p, flags);
}

static int ProcFsStatPathCb(void* mountPriv, uint8_t /*pdrv*/,
                            const char* relPath, VnodeStat* st)
{
    if (mountPriv) return -95;
    const char* p = (relPath && relPath[0] == '/') ? relPath + 1 : relPath;
    if (!p || !p[0]) { st->size = 0; st->isDir = true; return 0; }
    return ProcFsStatPath(p, st);
}

static const VfsFsOps g_procFsOps = {
    .mount      = ProcFsMount,
    .unmount    = ProcFsUnmountCb,
    .open       = ProcFsOpenCb,
    .stat_path  = ProcFsStatPathCb,
    .lstat_path = nullptr,
    .unlink     = nullptr,
    .mkdir      = nullptr,
    .rename     = nullptr,
    .symlink    = nullptr,
    .readlink   = nullptr,
    .sync       = nullptr,
    .opaque_symlinks = true,
    .root_object=ProcFsRootObject,
    .vnode_object=ProcFsVnodeObject,
};

static bool ProcMountCovers(const void* existing, const void* requested)
{
    const auto* current = static_cast<const ProcMountView*>(existing);
    const auto* fresh = static_cast<const ProcMountView*>(requested);
    return PidNamespaceContains(current ? current->pid : nullptr, fresh->pid);
}

int ProcFsMountPrivate(MountNamespace* ns,const VfsPath& root,const VfsPath& target,uint64_t attributes)
{
    auto* process = ProcessCurrent();
    const auto* pidView = ProcessPidView(process);
    if (!ProcessCapable(process, PidNamespaceOwner(pidView), CapSysAdmin)) return -1;

    auto* instance = static_cast<ProcMountView*>(kmalloc(sizeof(ProcMountView)));
    if (!instance) return -12;
    instance->pid = pidView;
    PidNamespaceRetain(instance->pid);
    const VfsFreshMountVisibility visibility{root, ProcMountCovers};
    const int error = VfsMountNamespaceMountFresh(ns,target,"procfs",&g_procFsOps,instance,0,
        attributes|MountNoSuid|MountNoDev|MountNoExec,nullptr,&visibility);
    if (error) ProcFsUnmountCb(instance);
    return error;
}

void ProcFsVfsRegister()
{
    VfsRegisterFs("procfs", &g_procFsOps);
}

} // namespace brook
