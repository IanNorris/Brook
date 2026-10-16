#include "vfs.h"
#include "memory/file_object.h"
#include "memory/mapping_pin_registry.h"
#include "panic.h"
#include "procfs.h"
#include "memory/heap.h"
#include "serial.h"
#include "memory/virtual_memory.h"
#include "sync/kmutex.h"
#include "spinlock.h"

// Forward-declare SchedulerYield — weak so test builds link without the scheduler.
namespace brook { __attribute__((weak)) void SchedulerYield(); }

namespace brook {

// Global lock protecting VFS mount table and filesystem operations.
static KMutex g_vfsLock;

// Namespace changes, initial mapping registration and resize are serialized.
// Fork/split/unmap only touch the short registry spinlock below.
static KMutex g_fileMutationLock;
struct FileMutationGuard {
    bool held;
    explicit FileMutationGuard(bool needed = true) : held(needed) { if (held) KMutexLock(&g_fileMutationLock); }
    ~FileMutationGuard() { if (held) KMutexUnlock(&g_fileMutationLock); }
};
static bool VfsMappedPathBusy(const char* path);
static int VfsTruncateLocked(Vnode* vn, uint64_t size);

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
    char             fsType[16];     // filesystem type name (e.g. "fatfs", "ext2")
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

static void StrAppend(char* dst, const char* src, uint32_t maxLen)
{
    uint32_t di = StrLen(dst);
    uint32_t si = 0;
    while (di + 1 < maxLen && src[si])
        dst[di++] = src[si++];
    dst[di] = '\0';
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

static int VfsLstatPathRaw(const char* path, VnodeStat* st)
{
    if (!path || !st) return -1;
    *st = {};

    const char* relPath = nullptr;
    MountEntry* mount   = FindMount(path, &relPath);
    if (!mount || !mount->fsOps) return -1;

    if (mount->fsOps->lstat_path)
        return mount->fsOps->lstat_path(mount->mountPriv, mount->pdrv, relPath, st);
    if (mount->fsOps->stat_path)
        return mount->fsOps->stat_path(mount->mountPriv, mount->pdrv, relPath, st);
    return -1;
}

static int VfsReadlinkRaw(const char* path, char* buf, uint64_t bufsiz)
{
    if (!path || !buf || bufsiz == 0) return -22;
    const char* relPath = nullptr;
    MountEntry* mount = FindMount(path, &relPath);
    if (!mount || !mount->fsOps) return -2;
    // A filesystem root is a directory, including when reached through a
    // mountpoint like /data. Backend parent/name lookup cannot represent it.
    if (relPath[0]=='/' && !relPath[1]) return -22;
    if (!mount->fsOps->readlink) {
        VnodeStat stat{};
        const int result=VfsLstatPathRaw(path,&stat);
        return result ? (result==-1?-2:result) : -22;
    }
    return mount->fsOps->readlink(mount->mountPriv, mount->pdrv, relPath, buf, bufsiz);
}

static bool NormalizePath(const char* in, char* out, uint32_t outSize)
{
    if (!in || !out || outSize < 2) return false;

    out[0] = '/';
    out[1] = '\0';

    const char* p = in;
    while (*p)
    {
        while (*p == '/') ++p;
        if (!*p) break;

        char comp[256];
        uint32_t ci = 0;
        while (*p && *p != '/' && ci + 1 < sizeof(comp))
            comp[ci++] = *p++;
        comp[ci] = '\0';
        while (*p && *p != '/') ++p;

        if (StrEq(comp, "."))
            continue;

        if (StrEq(comp, ".."))
        {
            uint32_t len = StrLen(out);
            if (len > 1)
            {
                if (out[len - 1] == '/') out[--len] = '\0';
                while (len > 1 && out[len - 1] != '/') --len;
                out[len == 1 ? 1 : len] = '\0';
            }
            continue;
        }

        if (!StrEq(out, "/")) StrAppend(out, "/", outSize);
        StrAppend(out, comp, outSize);
    }

    return true;
}

static bool BuildSymlinkPath(const char* linkPath, const char* target,
                             const char* remaining, char* out, uint32_t outSize)
{
    char combined[512];
    combined[0] = '\0';

    if (target[0] == '/')
    {
        StrCopy(combined, target, sizeof(combined));
    }
    else
    {
        StrCopy(combined, linkPath, sizeof(combined));
        uint32_t len = StrLen(combined);
        while (len > 1 && combined[len - 1] != '/') --len;
        combined[len] = '\0';
        if (!StrEq(combined, "/")) StrAppend(combined, "/", sizeof(combined));
        StrAppend(combined, target, sizeof(combined));
    }

    if (remaining && remaining[0])
    {
        if (!StrEq(combined, "/")) StrAppend(combined, "/", sizeof(combined));
        StrAppend(combined, remaining, sizeof(combined));
    }

    return NormalizePath(combined, out, outSize);
}

static bool VfsResolveSymlinks(const char* path, char* out, uint32_t outSize,
                               bool followFinal)
{
    if (!path || !path[0] || !out || outSize < 2) return false;

    char work[512];
    if (!NormalizePath(path, work, sizeof(work))) return false;

    for (int depth = 0; depth < 8; ++depth)
    {
        char cur[512];
        cur[0] = '/';
        cur[1] = '\0';
        const char* p = work;
        while (*p == '/') ++p;

        while (*p)
        {
            char comp[256];
            uint32_t ci = 0;
            while (*p && *p != '/' && ci + 1 < sizeof(comp))
                comp[ci++] = *p++;
            comp[ci] = '\0';
            while (*p == '/') ++p;

            if (!StrEq(cur, "/")) StrAppend(cur, "/", sizeof(cur));
            StrAppend(cur, comp, sizeof(cur));

            bool final = (*p == '\0');
            if (!final || followFinal)
            {
                VnodeStat st;
                st.size = 0;
                st.isDir = false;
                st.isSymlink = false;
                if (VfsLstatPathRaw(cur, &st) == 0 && st.isSymlink)
                {
                    char target[512];
                    int n = VfsReadlinkRaw(cur, target, sizeof(target) - 1);
                    if (n < 0) return false;
                    target[n] = '\0';

                    char next[512];
                    if (!BuildSymlinkPath(cur, target, p, next, sizeof(next)))
                        return false;
                    StrCopy(work, next, sizeof(work));
                    goto restart;
                }
            }
        }

        StrCopy(out, cur, outSize);
        return true;

    restart:
        continue;
    }

    return false;
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
    StrCopy(slot->fsType, fsName, sizeof(slot->fsType));

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

        if (g_mounts[i].fsOps && g_mounts[i].fsOps->can_unmount &&
            !g_mounts[i].fsOps->can_unmount(g_mounts[i].mountPriv))
            return false;

        if (g_mounts[i].fsOps && g_mounts[i].fsOps->unmount &&
            g_mounts[i].fsOps->unmount(g_mounts[i].mountPriv) != 0)
            return false;

        g_mounts[i].used = false;
        SerialPrintf("VFS: unmounted '%s'\n", mountPoint);
        return true;
    }
    return false;
}

uint32_t VfsRootMountCount()
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; ++i)
    {
        if (!g_mounts[i].used) continue;
        const char* mp = g_mounts[i].mountPoint;
        if (mp[0] != '/' || mp[1] == '\0') continue;

        bool nested = false;
        for (uint32_t j = 1; mp[j]; ++j)
        {
            if (mp[j] == '/') { nested = true; break; }
        }
        if (!nested) ++count;
    }
    return count;
}

bool VfsRootMountNameAt(uint32_t index, char* out, uint32_t outSize)
{
    if (!out || outSize == 0) return false;

    uint32_t seen = 0;
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; ++i)
    {
        if (!g_mounts[i].used) continue;
        const char* mp = g_mounts[i].mountPoint;
        if (mp[0] != '/' || mp[1] == '\0') continue;

        bool nested = false;
        uint32_t nameLen = 0;
        for (uint32_t j = 1; mp[j]; ++j)
        {
            if (mp[j] == '/') { nested = true; break; }
            ++nameLen;
        }
        if (nested || nameLen == 0) continue;

        if (seen++ != index) continue;

        uint32_t copyLen = (nameLen < outSize - 1) ? nameLen : outSize - 1;
        for (uint32_t j = 0; j < copyLen; ++j)
            out[j] = mp[j + 1];
        out[copyLen] = '\0';
        return true;
    }
    return false;
}

uint32_t VfsMountMaxSlots() { return VFS_MAX_MOUNTS; }

VfsMountSnapshot VfsMountSnapshotAt(uint32_t idx)
{
    VfsMountSnapshot snap = {};
    if (idx >= VFS_MAX_MOUNTS) return snap;
    const auto& m = g_mounts[idx];
    snap.used = m.used;
    if (!m.used) return snap;
    StrCopy(snap.mountPoint, m.mountPoint, sizeof(snap.mountPoint));
    StrCopy(snap.fsType, m.fsType, sizeof(snap.fsType));
    return snap;
}

uint32_t VfsFsMaxSlots() { return VFS_MAX_FS_DRIVERS; }

VfsFsSnapshot VfsFsSnapshotAt(uint32_t idx)
{
    VfsFsSnapshot snap = {};
    if (idx >= VFS_MAX_FS_DRIVERS) return snap;
    snap.used = g_fsDrivers[idx].used;
    if (!snap.used) return snap;
    const char* src = g_fsDrivers[idx].name;
    uint32_t j = 0;
    for (; j < 15 && src && src[j]; ++j) snap.name[j] = src[j];
    snap.name[j] = '\0';
    return snap;
}

int VfsOpenExecutable(const char* path, Vnode** vnode, char* resolved, uint32_t size)
{
    if (!vnode || !resolved || size < 2) return -22;
    *vnode = nullptr;
    resolved[0] = 0;
    if (!path || path[0] != '/') return -2;
    char remaining[512], current[512];
    current[0] = '/';
    current[1] = 0;
    if (StrLen(path) >= sizeof(remaining)) return -36;
    StrCopy(remaining, path, sizeof(remaining));
    uint32_t links = 0;
    for (;;) {
        const char* next = remaining;
        while (*next == '/') ++next;
        if (!*next) break;
        char component[256];
        uint32_t count = 0;
        while (*next && *next != '/') {
            if (count + 1 >= sizeof(component)) return -36;
            component[count++] = *next++;
        }
        component[count] = 0;
        char tail[512];
        StrCopy(tail, next, sizeof(tail));
        if (StrEq(component, ".")) {
            StrCopy(remaining, tail, sizeof(remaining));
            continue;
        }
        if (StrEq(component, "..")) {
            uint32_t n = StrLen(current);
            while (n > 1 && current[n - 1] != '/') --n;
            if (n > 1) --n;
            current[n] = 0;
            StrCopy(remaining, tail, sizeof(remaining));
            continue;
        }
        uint32_t parentLength = StrLen(current);
        uint32_t slash = parentLength > 1 ? 1 : 0;
        if (parentLength + slash + count >= sizeof(current)) return -36;
        if (slash) StrAppend(current, "/", sizeof(current));
        StrAppend(current, component, sizeof(current));
        VnodeStat st{};
        if (VfsLstatPathRaw(current, &st) != 0) return -2;
        if (st.isSymlink) {
            if (++links > 40) return -40;
            char target[512];
            int bytes = VfsReadlinkRaw(current, target, sizeof(target) - 1);
            if (bytes < 0) return bytes;
            // readlink can truncate without a terminator; refuse ambiguity.
            if (bytes >= static_cast<int>(sizeof(target) - 1)) return -36;
            if (!bytes) return -2;
            target[bytes] = 0;
            if (static_cast<uint32_t>(bytes) + StrLen(tail) >= sizeof(remaining)) return -36;
            StrCopy(remaining, target, sizeof(remaining));
            StrAppend(remaining, tail, sizeof(remaining));
            if (target[0] == '/') { current[0] = '/'; current[1] = 0; }
            else current[parentLength] = 0;
            continue;
        }
        // Resolve '..' only AFTER symlinks, and require directory traversal
        // even when the remaining suffix consists solely of a trailing slash.
        if (tail[0] && !st.isDir) return -20;
        StrCopy(remaining, tail, sizeof(remaining));
    }
    if (StrLen(current) >= size) return -36;
    const char* relative = nullptr;
    MountEntry* mount = FindMount(current, &relative);
    if (!mount || !mount->fsOps) return -2;
    Vnode* opened = nullptr;
    if (mount->fsOps->open_mode)
        opened = mount->fsOps->open_mode(mount->mountPriv, mount->pdrv, relative, 0, 0666);
    else if (mount->fsOps->open)
        opened = mount->fsOps->open(mount->mountPriv, mount->pdrv, relative, 0);
    if (!opened) return -2;
    if (opened->type == VnodeType::Dir) { VfsClose(opened); return -13; }
    StrCopy(resolved, current, size);
    *vnode = opened;
    return 0;
}

Vnode* VfsOpen(const char* path, int flags)
{
    return VfsOpenMode(path, flags, 0666);
}

Vnode* VfsOpenMode(const char* path, int flags, uint16_t mode)
{
    return VfsOpenModeChecked(path, flags, mode, nullptr);
}

Vnode* VfsOpenModeChecked(const char* path, int flags, uint16_t mode, int* error)
{
    if (error) *error = 0;
    FileMutationGuard mutation((flags & (VFS_O_TRUNC | VFS_O_CREATE)) != 0);
    if (!path || !path[0]) return nullptr;

    // Check the entry itself before following the final symlink: O_EXCL also
    // rejects dangling symlinks. The namespace mutation gate spans check/create
    // and is shared by regular creation, mkdir, symlink, rename and unlink.
    char resolved[512];
    const char* lookup = path;
    const bool exclusive = (flags & (VFS_O_CREATE | VFS_O_EXCL)) ==
                            (VFS_O_CREATE | VFS_O_EXCL);
    if (exclusive) {
        if (VfsResolveSymlinks(path, resolved, sizeof(resolved), false)) lookup = resolved;
        VnodeStat stat{};
        if (VfsLstatPathRaw(lookup, &stat) == 0) {
            if (error) *error = -17;
            return nullptr;
        }
    } else if (VfsResolveSymlinks(path, resolved, sizeof(resolved), true)) {
        lookup = resolved;
    }

    const char* relPath = nullptr;
    MountEntry* mount   = FindMount(lookup, &relPath);
    if (!mount)
    {
        SerialPrintf("VFS: no mount for '%s'\n", lookup);
        return nullptr;
    }

    if (!mount->fsOps) return nullptr;
    // O_TRUNC must use the same resize gate and alias invalidation as truncate.
    int openFlags = flags & ~(VFS_O_TRUNC | VFS_O_EXCL);
    Vnode* vn = nullptr;
    if (mount->fsOps->open_mode)
        vn = mount->fsOps->open_mode(mount->mountPriv, mount->pdrv, relPath, openFlags, mode);
    else if (mount->fsOps->open)
        vn = mount->fsOps->open(mount->mountPriv, mount->pdrv, relPath, openFlags);
    if (vn && (flags & VFS_O_TRUNC)) {
        int result = VfsTruncateLocked(vn, 0);
        if (result) {
            if (error) *error = result;
            VfsClose(vn);
            return nullptr;
        }
    }
    return vn;
}

int VfsChmod(Vnode* vn, uint16_t mode)
{
    if (!vn) return -9;
    if (!vn->ops || !vn->ops->chmod) return -95;
    return vn->ops->chmod(vn, mode);
}

int VfsChmodPath(const char* path, uint16_t mode)
{
    if (!path || !path[0]) return -2;
    char resolved[512];
    const char* lookup = path;
    if (VfsResolveSymlinks(path, resolved, sizeof(resolved), true)) lookup = resolved;
    const char* relPath = nullptr;
    MountEntry* mount = FindMount(lookup, &relPath);
    if (!mount) return -2;
    if (!mount->fsOps || !mount->fsOps->chmod) return -95;
    return mount->fsOps->chmod(mount->mountPriv, mount->pdrv, relPath, mode);
}

int VfsChown(Vnode* vn, uint32_t uid, uint32_t gid)
{
    if (!vn) return -9;
    if (!vn->ops || !vn->ops->chown) return -95;
    return vn->ops->chown(vn, uid, gid);
}

int VfsChownPath(const char* path, uint32_t uid, uint32_t gid, bool nofollow)
{
    if (!path || !path[0]) return -2;
    char resolved[512];
    const char* lookup = path;
    if (VfsResolveSymlinks(path, resolved, sizeof(resolved), !nofollow)) lookup = resolved;
    const char* relPath = nullptr;
    MountEntry* mount = FindMount(lookup, &relPath);
    if (!mount) return -2;
    if (!mount->fsOps || !mount->fsOps->chown) return -95;
    return mount->fsOps->chown(mount->mountPriv, mount->pdrv, relPath, uid, gid, nofollow);
}

int VfsRead(Vnode* vn, void* buf, uint64_t len, uint64_t* offset)
{
    if (!vn) return -1;
    // Detect use-after-free: kfree fills with 0xDF
    uintptr_t opsVal = reinterpret_cast<uintptr_t>(vn->ops);
    if (opsVal == 0xdfdfdfdfdfdfdfdfULL) {
        SerialPrintf("BUG: VfsRead on freed vnode %p (ops=POISON refCount=%u)\n",
                     vn, vn->refCount);
        return -1;
    }
    if (!vn->ops->read) return -1;
    if(vn->ops->file_object)
        return FileObjectRead(vn->ops->file_object(vn),vn,buf,len,offset);
    return vn->ops->read(vn, buf, len, offset);
}

int VfsWrite(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset)
{
    if (!vn || !vn->ops->write) return -1;
    if(vn->ops->file_object)
        return FileObjectWrite(vn->ops->file_object(vn),vn,buf,len,offset);
    return vn->ops->write(vn, buf, len, offset);
}

// Mapping descriptors retain their vnode. Dynamic bindings aggregate separate
// opens by stable identity without a fixed number of simultaneous mappings.
static MappingPinRegistry g_mappingPins;

int VfsFileMapPin(Vnode* vn)
{
    if (g_mappingPins.RetainIfPinned(vn)) return 0;
    FileMutationGuard mutation;
    VnodeStat st;
    int result = VfsStat(vn, &st);
    if (result) return result;
    return g_mappingPins.Pin(vn,st.dev,st.ino);
}

// Existing descriptor pin required; no allocation, filesystem I/O or mutex.
void VfsFileMapClone(Vnode* vn)
{
    if (!g_mappingPins.RetainIfPinned(vn)) KernelPanic("VFS: cloning absent mapping pin");
}

void VfsFileMapUnpin(Vnode* vn)
{
    g_mappingPins.Unpin(vn);
}

static int VfsTruncateLocked(Vnode* vn, uint64_t size)
{
    if (!vn) return -9;
    if (size > 0x7fffffffffffffffULL) return -22;
    if (vn->type == VnodeType::Dir) return -21;
    if (!vn->ops || !vn->ops->truncate) return -95;
    VnodeStat st;
    int result = VfsStat(vn, &st);
    if (result) return result;
    // FileMutationGuard excludes first-pin publication through backend resize.
    // The allocation-free fast path can only retain an already pinned binding,
    // which would make this identity busy. No resize placeholder is necessary.
    if (g_mappingPins.Busy(st.dev,st.ino)) return -16;
    result = vn->ops->file_object ?
        FileObjectTruncateUnmapped(vn->ops->file_object(vn),vn,size) : vn->ops->truncate(vn,size);
    return result;
}

int VfsTruncate(Vnode* vn, uint64_t size)
{
    FileMutationGuard mutation;
    return VfsTruncateLocked(vn, size);
}

static bool VfsMappedPathBusy(const char* path)
{
    VnodeStat st;
    if (VfsLstatPath(path, &st)) return false;
    // ext2 open-inode holders preserve deleted/renamed backing. FAT has no
    // persistent inode and must retain its physical directory entry instead.
    if (((st.dev >> 8) & 0xfff) != 253) return false;
    return g_mappingPins.Busy(st.dev,st.ino,st.isDir);
}


int VfsReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie)
{
    if (!vn || !vn->ops->readdir) return -1;
    return vn->ops->readdir(vn, out, cookie);
}

int VfsStat(Vnode* vn, VnodeStat* st)
{
    if (!vn || !vn->ops->stat || !st) return -1;
    *st = {};
    int ret = vn->ops->stat(vn, st);
    if (ret == 0 && !st->ino)
        st->ino = vn->cacheId;
    return ret;
}

int VfsStatPath(const char* path, VnodeStat* st)
{
    if (!path || !path[0] || !st) return -1;
    *st = {};

    char resolved[512];
    const char* lookup = path;
    if (VfsResolveSymlinks(path, resolved, sizeof(resolved), true))
        lookup = resolved;

    const char* relPath = nullptr;
    MountEntry* mount   = FindMount(lookup, &relPath);
    if (!mount) return -1;

    if (!mount->fsOps || !mount->fsOps->stat_path) return -1;
    return mount->fsOps->stat_path(mount->mountPriv, mount->pdrv, relPath, st);
}

int VfsLstatPath(const char* path, VnodeStat* st)
{
    if (!path || !st) return -1;
    *st = {};

    char resolved[512];
    const char* lookup = path;
    if (VfsResolveSymlinks(path, resolved, sizeof(resolved), false))
        lookup = resolved;

    const char* relPath = nullptr;
    MountEntry* mount   = FindMount(lookup, &relPath);
    if (!mount || !mount->fsOps) return -1;

    // Use lstat_path if available, otherwise fall back to stat_path
    if (mount->fsOps->lstat_path)
        return mount->fsOps->lstat_path(mount->mountPriv, mount->pdrv, relPath, st);
    if (mount->fsOps->stat_path)
        return mount->fsOps->stat_path(mount->mountPriv, mount->pdrv, relPath, st);
    return -1;
}

int VfsSetTimes(Vnode* vn, const VnodeTime* times)
{
    if (!vn || !vn->ops || !times) return -22;
    if (!vn->ops->set_times) return -95; // EOPNOTSUPP
    return vn->ops->set_times(vn, times);
}

int VfsSetTimesPath(const char* path, const VnodeTime* times, bool nofollow)
{
    if (!path || !path[0] || !times) return -22;
    char resolved[512];
    const char* lookup = path;
    if (VfsResolveSymlinks(path, resolved, sizeof(resolved), !nofollow))
        lookup = resolved;
    const char* relPath = nullptr;
    MountEntry* mount = FindMount(lookup, &relPath);
    if (!mount || !mount->fsOps) return -2;
    if (!mount->fsOps->set_times) return -95;
    return mount->fsOps->set_times(mount->mountPriv, mount->pdrv, relPath, times, nofollow);
}

int VfsSync(Vnode* vn)
{
    if (vn) return VfsFsync(vn);
    // Try every mount even after a failure, but never acknowledge a failed
    // filesystem/device flush as successful to sync(2) or the VM bridge.
    int result = FileObjectWritebackAll();
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; ++i) {
        if (!g_mounts[i].used) continue;
        if (g_mounts[i].fsOps && g_mounts[i].fsOps->sync) {
            int error = g_mounts[i].fsOps->sync(g_mounts[i].mountPriv);
            if (!result && error) result = error < 0 ? error : -5;
        }
    }
    return result;
}

int VfsFsync(Vnode* vn)
{
    // Per-file durability: persist this file's data + on-disk directory
    // entry (size/clusters).  Used by long-lived writers (e.g. the profiler)
    // that must survive an abrupt poweroff before an explicit close.
    if (!vn || !vn->ops || !vn->ops->fsync) return -95;
    int result=0;
    if(vn->ops->file_object) {
        if(auto* object=vn->ops->file_object(vn))result=FileObjectWriteback(object,vn,0,~uint64_t{0});
    }
    const int flush=vn->ops->fsync(vn);
    return result?result:flush;
}

int VfsUnlink(const char* path)
{
    FileMutationGuard mutation;
    if (VfsMappedPathBusy(path)) return -16;
    if (!path || !path[0]) return -1;
    const char* relPath = nullptr;
    MountEntry* mount = FindMount(path, &relPath);
    if (!mount || !mount->fsOps || !mount->fsOps->unlink) return -1;
    return mount->fsOps->unlink(mount->mountPriv, mount->pdrv, relPath);
}

int VfsRemovePath(const char* path, bool directory)
{
    FileMutationGuard mutation;
    if (VfsMappedPathBusy(path)) return -16;
    if (!path || !path[0]) return -2;
    const char* relPath = nullptr;
    MountEntry* mount = FindMount(path, &relPath);
    if (!mount || !mount->fsOps) return -2;
    if (mount->fsOps->remove_path)
        return mount->fsOps->remove_path(mount->mountPriv, mount->pdrv, relPath, directory);
    VnodeStat st;
    if (VfsLstatPath(path, &st) != 0) return -2;
    if (st.isDir != directory) return directory ? -20 : -21;
    if (!mount->fsOps->unlink) return -95;
    return mount->fsOps->unlink(mount->mountPriv, mount->pdrv, relPath);
}

int VfsMkdir(const char* path, uint16_t mode)
{
    FileMutationGuard mutation;
    if (!path || !path[0]) return -1;
    const char* relPath = nullptr;
    MountEntry* mount = FindMount(path, &relPath);
    if (!mount || !mount->fsOps || !mount->fsOps->mkdir) return -1;
    return mount->fsOps->mkdir(mount->mountPriv, mount->pdrv, relPath, mode);
}

int VfsRename(const char* oldPath, const char* newPath)
{
    FileMutationGuard mutation;
    if (VfsMappedPathBusy(oldPath) || VfsMappedPathBusy(newPath)) return -16;
    if (!oldPath || !newPath || !oldPath[0] || !newPath[0]) return -1;
    const char* relOld = nullptr;
    const char* relNew = nullptr;
    MountEntry* mountOld = FindMount(oldPath, &relOld);
    MountEntry* mountNew = FindMount(newPath, &relNew);
    if (!mountOld || !mountNew) return -2;
    if (mountOld != mountNew) return -18;
    if (!mountOld->fsOps || !mountOld->fsOps->rename) return -1;
    return mountOld->fsOps->rename(mountOld->mountPriv, mountOld->pdrv, relOld, relNew);
}

int VfsSymlink(const char* target, const char* linkPath)
{
    FileMutationGuard mutation;
    if (!target || !linkPath || !target[0] || !linkPath[0]) return -22; // -EINVAL
    const char* relPath = nullptr;
    MountEntry* mount = FindMount(linkPath, &relPath);
    if (!mount || !mount->fsOps || !mount->fsOps->symlink) return -1; // -EPERM
    return mount->fsOps->symlink(mount->mountPriv, mount->pdrv, target, relPath);
}

int VfsReadlink(const char* path, char* buf, uint64_t bufsiz)
{
    if (!path || !buf || bufsiz == 0) return -22;

    char resolved[512];
    const char* lookup = path;
    if (VfsResolveSymlinks(path, resolved, sizeof(resolved), false))
        lookup = resolved;

    return VfsReadlinkRaw(lookup, buf, bufsiz);
}

void VfsClose(Vnode* vn)
{
    if (!vn) return;
    if (vn->ops && vn->ops->close) vn->ops->close(vn);
    kfree(vn);
}

} // namespace brook
