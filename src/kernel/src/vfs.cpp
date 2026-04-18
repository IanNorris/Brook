#include "vfs.h"
#include "procfs.h"
#include "memory/heap.h"
#include "serial.h"
#include "memory/virtual_memory.h"
#include "sync/kmutex.h"

// Forward-declare SchedulerYield — weak so test builds link without the scheduler.
namespace brook { __attribute__((weak)) void SchedulerYield(); }

namespace brook {

// Global lock protecting VFS mount table and filesystem operations.
static KMutex g_vfsLock;

// ---- Filesystem driver registry ----

static constexpr uint32_t VFS_MAX_FS_DRIVERS = 8;

struct FsDriverEntry {
    const char*      name;
    const VfsFsOps*  ops;
    bool             used;
};

static FsDriverEntry g_fsDrivers[VFS_MAX_FS_DRIVERS];

static const VfsFsOps* FindFsDriver(const char* name)
{
    for (uint32_t i = 0; i < VFS_MAX_FS_DRIVERS; ++i)
    {
        if (!g_fsDrivers[i].used) continue;
        const char* a = g_fsDrivers[i].name;
        const char* b = name;
        while (*a && *b) { if (*a != *b) goto next; ++a; ++b; }
        if (*a == *b) return g_fsDrivers[i].ops;
    next:;
    }
    return nullptr;
}

bool VfsRegisterFs(const char* name, const VfsFsOps* ops)
{
    if (!name || !ops) return false;
    for (uint32_t i = 0; i < VFS_MAX_FS_DRIVERS; ++i)
    {
        if (!g_fsDrivers[i].used)
        {
            g_fsDrivers[i].name = name;
            g_fsDrivers[i].ops  = ops;
            g_fsDrivers[i].used = true;
            SerialPrintf("VFS: registered filesystem '%s'\n", name);
            return true;
        }
    }
    SerialPrintf("VFS: fs driver table full, cannot register '%s'\n", name);
    return false;
}

bool VfsUnregisterFs(const char* name)
{
    if (!name) return false;
    for (uint32_t i = 0; i < VFS_MAX_FS_DRIVERS; ++i)
    {
        if (!g_fsDrivers[i].used) continue;
        const char* a = g_fsDrivers[i].name;
        const char* b = name;
        while (*a && *b) { if (*a != *b) goto next2; ++a; ++b; }
        if (*a == *b) { g_fsDrivers[i].used = false; return true; }
    next2:;
    }
    return false;
}

// ---- Mount table ----

static constexpr uint32_t VFS_MAX_MOUNTS = 8;

struct MountEntry {
    char             mountPoint[64];
    const VfsFsOps*  fsOps;       // filesystem driver vtable
    void*            mountPriv;   // filesystem-private mount data
    uint8_t          pdrv;        // physical drive (for block FS)
    bool             used;
};

static MountEntry g_mounts[VFS_MAX_MOUNTS];

// ---- String helpers ----

static uint32_t StrLen(const char* s)
{
    uint32_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

static void StrCopy(char* dst, const char* src, uint32_t maxLen)
{
    uint32_t i = 0;
    while (i + 1 < maxLen && src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static bool StrEq(const char* a, const char* b)
{
    while (*a && *b) { if (*a != *b) return false; ++a; ++b; }
    return *a == *b;
}

// Find the best (longest-prefix) mount for a given absolute path.
// Returns the mount entry and sets *relPath to the path within the mount.
static MountEntry* FindMount(const char* absPath, const char** relPath)
{
    MountEntry* best      = nullptr;
    uint32_t    bestLen   = 0;

    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; ++i)
    {
        if (!g_mounts[i].used) continue;
        uint32_t mpLen = StrLen(g_mounts[i].mountPoint);
        bool match = true;
        for (uint32_t j = 0; j < mpLen; ++j) {
            if (absPath[j] != g_mounts[i].mountPoint[j]) { match = false; break; }
        }
        if (!match) continue;
        if (mpLen > 1 && absPath[mpLen] != '\0' && absPath[mpLen] != '/') continue;
        if (mpLen > bestLen) { bestLen = mpLen; best = &g_mounts[i]; }
    }

    if (best && relPath)
    {
        const char* rem = absPath + bestLen;
        *relPath = (rem[0] == '\0') ? "/" : rem;
    }
    return best;
}

// ---- Public API ----

void VfsInit()
{
    KMutexInit(&g_vfsLock);
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; ++i) g_mounts[i].used = false;
    for (uint32_t i = 0; i < VFS_MAX_FS_DRIVERS; ++i) g_fsDrivers[i].used = false;
    SerialPuts("VFS: initialised\n");
}

bool VfsMount(const char* mountPoint, const char* fsName, uint8_t pdrv)
{
    if (!mountPoint || !fsName) return false;

    const VfsFsOps* ops = FindFsDriver(fsName);
    if (!ops)
    {
        SerialPrintf("VFS: unknown filesystem '%s'\n", fsName);
        return false;
    }

    // Find a free slot.
    MountEntry* slot = nullptr;
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; ++i)
    {
        if (!g_mounts[i].used) { slot = &g_mounts[i]; break; }
    }
    if (!slot) { SerialPuts("VFS: mount table full\n"); return false; }

    // Call filesystem's mount callback
    void* priv = nullptr;
    if (!ops->mount(pdrv, &priv))
    {
        SerialPrintf("VFS: mount failed for '%s' at '%s'\n", fsName, mountPoint);
        return false;
    }

    slot->fsOps      = ops;
    slot->mountPriv  = priv;
    slot->pdrv       = pdrv;
    slot->used       = true;
    StrCopy(slot->mountPoint, mountPoint, sizeof(slot->mountPoint));

    DbgPrintf("VFS: mounted '%s' at '%s' (pdrv=%u)\n", fsName, mountPoint, pdrv);
    return true;
}

bool VfsUnmount(const char* mountPoint)
{
    if (!mountPoint) return false;
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; ++i)
    {
        if (!g_mounts[i].used) continue;
        if (!StrEq(g_mounts[i].mountPoint, mountPoint)) continue;

        if (g_mounts[i].fsOps && g_mounts[i].fsOps->unmount)
            g_mounts[i].fsOps->unmount(g_mounts[i].mountPriv);

        g_mounts[i].used = false;
        SerialPrintf("VFS: unmounted '%s'\n", mountPoint);
        return true;
    }
    return false;
}

Vnode* VfsOpen(const char* path, int flags)
{
    if (!path || !path[0]) return nullptr;

    const char* relPath = nullptr;
    MountEntry* mount   = FindMount(path, &relPath);
    if (!mount)
    {
        SerialPrintf("VFS: no mount for '%s'\n", path);
        return nullptr;
    }

    if (!mount->fsOps || !mount->fsOps->open) return nullptr;
    return mount->fsOps->open(mount->mountPriv, mount->pdrv, relPath, flags);
}

int VfsRead(Vnode* vn, void* buf, uint64_t len, uint64_t* offset)
{
    if (!vn || !vn->ops->read) return -1;
    int r = vn->ops->read(vn, buf, len, offset);
    return r;
}

int VfsWrite(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset)
{
    if (!vn || !vn->ops->write) return -1;
    return vn->ops->write(vn, buf, len, offset);
}

int VfsReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie)
{
    if (!vn || !vn->ops->readdir) return -1;
    return vn->ops->readdir(vn, out, cookie);
}

int VfsStat(Vnode* vn, VnodeStat* st)
{
    if (!vn || !vn->ops->stat) return -1;
    return vn->ops->stat(vn, st);
}

int VfsStatPath(const char* path, VnodeStat* st)
{
    if (!path || !path[0] || !st) return -1;

    const char* relPath = nullptr;
    MountEntry* mount   = FindMount(path, &relPath);
    if (!mount) return -1;

    if (!mount->fsOps || !mount->fsOps->stat_path) return -1;
    return mount->fsOps->stat_path(mount->mountPriv, mount->pdrv, relPath, st);
}

int VfsLstatPath(const char* path, VnodeStat* st)
{
    if (!path || !st) return -1;

    const char* relPath = nullptr;
    MountEntry* mount   = FindMount(path, &relPath);
    if (!mount || !mount->fsOps) return -1;

    // Use lstat_path if available, otherwise fall back to stat_path
    if (mount->fsOps->lstat_path)
        return mount->fsOps->lstat_path(mount->mountPriv, mount->pdrv, relPath, st);
    if (mount->fsOps->stat_path)
        return mount->fsOps->stat_path(mount->mountPriv, mount->pdrv, relPath, st);
    return -1;
}

int VfsSync(Vnode* vn)
{
    // Sync is vnode-ops based — handled by the filesystem's close/write ops.
    // For now, no generic sync path; filesystems implement it in their vnode ops.
    (void)vn;
    return 0;
}

int VfsUnlink(const char* path)
{
    if (!path || !path[0]) return -1;
    const char* relPath = nullptr;
    MountEntry* mount = FindMount(path, &relPath);
    if (!mount || !mount->fsOps || !mount->fsOps->unlink) return -1;
    return mount->fsOps->unlink(mount->mountPriv, mount->pdrv, relPath);
}

int VfsMkdir(const char* path)
{
    if (!path || !path[0]) return -1;
    const char* relPath = nullptr;
    MountEntry* mount = FindMount(path, &relPath);
    if (!mount || !mount->fsOps || !mount->fsOps->mkdir) return -1;
    return mount->fsOps->mkdir(mount->mountPriv, mount->pdrv, relPath);
}

int VfsRename(const char* oldPath, const char* newPath)
{
    if (!oldPath || !newPath || !oldPath[0] || !newPath[0]) return -1;
    const char* relOld = nullptr;
    const char* relNew = nullptr;
    MountEntry* mountOld = FindMount(oldPath, &relOld);
    MountEntry* mountNew = FindMount(newPath, &relNew);
    if (!mountOld || !mountNew || mountOld != mountNew) return -1;
    if (!mountOld->fsOps || !mountOld->fsOps->rename) return -1;
    return mountOld->fsOps->rename(mountOld->mountPriv, mountOld->pdrv, relOld, relNew);
}

int VfsSymlink(const char* target, const char* linkPath)
{
    if (!target || !linkPath || !target[0] || !linkPath[0]) return -1;
    const char* relPath = nullptr;
    MountEntry* mount = FindMount(linkPath, &relPath);
    if (!mount || !mount->fsOps || !mount->fsOps->symlink) return -1;
    return mount->fsOps->symlink(mount->mountPriv, mount->pdrv, target, relPath);
}

int VfsReadlink(const char* path, char* buf, uint64_t bufsiz)
{
    if (!path || !buf || bufsiz == 0) return -1;
    const char* relPath = nullptr;
    MountEntry* mount = FindMount(path, &relPath);
    if (!mount || !mount->fsOps || !mount->fsOps->readlink) return -1;
    return mount->fsOps->readlink(mount->mountPriv, mount->pdrv, relPath, buf, bufsiz);
}

void VfsClose(Vnode* vn)
{
    if (!vn) return;
    if (vn->ops && vn->ops->close) vn->ops->close(vn);
    kfree(vn);
}

} // namespace brook
