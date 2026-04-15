#include "procfs_vfs.h"
#include "vfs.h"
#include "procfs.h"
#include "memory/heap.h"

namespace brook {

static bool ProcFsMount(uint8_t /*pdrv*/, void** mountPriv)
{
    *mountPriv = nullptr;
    return true;
}

static void ProcFsUnmountCb(void* /*mountPriv*/) {}

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
    .mount     = ProcFsMount,
    .unmount   = ProcFsUnmountCb,
    .open      = ProcFsOpenCb,
    .stat_path = ProcFsStatPathCb,
    .unlink    = nullptr,
    .mkdir     = nullptr,
    .rename    = nullptr,
};

void ProcFsVfsRegister()
{
    VfsRegisterFs("procfs", &g_procFsOps);
}

} // namespace brook
