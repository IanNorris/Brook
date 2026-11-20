#include "procfs_vfs.h"
#include "vfs.h"
#include "mount_namespace.h"
#include "procfs.h"
#include "memory/heap.h"

namespace brook {

static bool ProcFsMount(uint8_t /*pdrv*/, void** mountPriv)
{
    *mountPriv = nullptr;
    return true;
}

static int ProcFsUnmountCb(void* /*mountPriv*/) { return 0; }

static Vnode* ProcFsOpenCb(void* /*mountPriv*/, uint8_t /*pdrv*/,
                           const char* relPath, int flags)
{
    if (!relPath || !relPath[0] || (relPath[0] == '/' && !relPath[1]))
        return ProcFsOpenDir(relPath);
    const char* p = relPath;
    if (p[0] == '/') p++;
    return ProcFsOpen(p, flags);
}

static int ProcFsStatPathCb(void* /*mountPriv*/, uint8_t /*pdrv*/,
                            const char* relPath, VnodeStat* st)
{
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

int ProcFsMountPrivate(MountNamespace* ns,const VfsPath& target,uint64_t attributes)
{
    // Proc objects query the caller's process view. This token separates
    // backing lifetimes without presenting an additional system process table.
    static uint8_t instanceToken;
    return VfsMountNamespaceMountFresh(ns,target,"procfs",&g_procFsOps,&instanceToken,0,
        attributes|MountNoSuid|MountNoDev|MountNoExec);
}

void ProcFsVfsRegister()
{
    VfsRegisterFs("procfs", &g_procFsOps);
}

} // namespace brook
