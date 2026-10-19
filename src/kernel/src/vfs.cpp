#include "vfs.h"
#include "memory/file_object.h"
#include "memory/mapping_pin_registry.h"
#include "panic.h"
#include "procfs.h"
#include "memory/heap.h"
#include "serial.h"
#include "memory/virtual_memory.h"
#include "sync/krwlock.h"
#include "path_walk.h"
#include "spinlock.h"

// Forward-declare SchedulerYield — weak so test builds link without the scheduler.
namespace brook { __attribute__((weak)) void SchedulerYield(); }

namespace brook {

// Namespace changes, initial mapping registration and resize are serialized.
// Fork/split/unmap only touch the short registry spinlock below.
static KRwLock g_fileMutationLock;
struct FileMutationGuard {
    bool write;
    explicit FileMutationGuard(bool mutation = true) : write(mutation) {
        if (write) KRwLockWriteLock(&g_fileMutationLock);
        else KRwLockReadLock(&g_fileMutationLock);
    }
    ~FileMutationGuard() {
        if (write) KRwLockWriteUnlock(&g_fileMutationLock);
        else KRwLockReadUnlock(&g_fileMutationLock);
    }
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
    if (!mount || !mount->fsOps) return -2;

    int result = -2;
    if (mount->fsOps->lstat_path)
        result = mount->fsOps->lstat_path(mount->mountPriv, mount->pdrv, relPath, st);
    else if (mount->fsOps->stat_path)
        result = mount->fsOps->stat_path(mount->mountPriv, mount->pdrv, relPath, st);
    // Legacy stat callbacks use -1 for missing entries, not authorization.
    if (result == -1) result = -2;
    if (result == -2) {
        // Mounts can have virtual ancestors (the boot probe uses /mnt/probe
        // without a physical /mnt). Do not fabricate ordinary missing paths or
        // replace a backing error/file: only an absent ancestor of a live mount.
        uint32_t length = StrLen(path);
        for (const auto& candidate : g_mounts) {
            if (!candidate.used || StrLen(candidate.mountPoint) <= length) continue;
            bool match = true;
            for (uint32_t i = 0; i < length; ++i)
                if (path[i] != candidate.mountPoint[i]) { match = false; break; }
            if (match && candidate.mountPoint[length] == '/') {
                *st = {}; st->isDir = true; st->mode = 0040555; return 0;
            }
        }
    }
    return result;
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

static int VfsCheckAccessRaw(const char* path, const VnodeStat& st, uint8_t required, bool realIds = false)
{
    MountEntry* mount = FindMount(path, nullptr);
    if (!mount || !mount->fsOps) return -2;
    auto policy = realIds ? mount->fsOps->check_access_real : mount->fsOps->check_access;
    if (policy) return policy(mount->mountPriv, &st, required);
    // Never substitute effective-ID authorization for a real-ID inquiry.
    if (realIds && mount->fsOps->check_access) return -95;
    return 0; // Backends without a policy retain their explicit legacy behavior.
}
struct VfsPathProvider {
    bool realIds;
    int Stat(const char* p, VnodeStat& st) {
        int error=VfsLstatPathRaw(p,&st);
        MountEntry* mount=FindMount(p,nullptr);
        // The walk cannot turn procfs FD objects into textual symlink targets.
        // Only its internal node is opaque; final stat/lstat keep backend data.
        if (!error && mount && mount->fsOps->opaque_symlinks) st.isSymlink=false;
        return error;
    }
    int Search(const char* p, const VnodeStat& st) { return VfsCheckAccessRaw(p,st,1,realIds); }
    int Readlink(const char* p,char* buf,uint32_t size) { return VfsReadlinkRaw(p,buf,size); }
};
// Caller holds the namespace lock, shared for lookups and exclusive for changes.
static int VfsResolvePath(const char* path,char* out,uint32_t size,PathWalkOptions options={},bool realIds=false)
{
    VfsPathProvider provider{realIds};
    return WalkPath<VnodeStat>(path,out,size,provider,options);
}
static bool VfsTrailingSlash(const char* path) { uint32_t n=StrLen(path);return n>1 && path[n-1]=='/'; }

// ---- Public API ----

void VfsInit()
{
    KRwLockInit(&g_fileMutationLock);
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; ++i) g_mounts[i].used = false;
    for (uint32_t i = 0; i < VFS_MAX_FS_DRIVERS; ++i) g_fsDrivers[i].used = false;
    SerialPuts("VFS: initialised\n");
}

bool VfsMount(const char* mountPoint, const char* fsName, uint8_t pdrv)
{
    FileMutationGuard mutation;
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
    FileMutationGuard mutation;
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

int VfsAccessPath(const char* path,uint8_t required,bool realIds,bool nofollow)
{
    if (required & ~7u) return -22;
    FileMutationGuard lookup(false);
    char resolved[512];
    if (int error=VfsResolvePath(path,resolved,sizeof(resolved),{!nofollow},realIds)) return error;
    VnodeStat st{};
    if (int error=VfsLstatPathRaw(resolved,&st)) return error;
    return VfsCheckAccessRaw(resolved,st,required,realIds);
}

int VfsAccess(Vnode* vn,uint8_t required,bool realIds)
{
    if (!vn) return -9;
    if (required & ~7u) return -22;
    FileMutationGuard lookup(false);
    if (vn->ops && vn->ops->access) return vn->ops->access(vn,required,realIds);
    return required ? -95 : 0;
}

int VfsResolveDirectory(const char* path,char* resolved,uint32_t size)
{
    FileMutationGuard lookup(false);
    if (int error=VfsResolvePath(path,resolved,size)) return error;
    VnodeStat st{};
    if (int error=VfsLstatPathRaw(resolved,&st)) return error;
    if (!st.isDir) return -20;
    return VfsCheckAccessRaw(resolved,st,1);
}

int VfsOpenExecutable(const char* path, Vnode** vnode, char* resolved, uint32_t size)
{
    if (!vnode || !resolved || size<2) return -22;
    *vnode=nullptr;resolved[0]=0;
    if (!path || path[0]!='/') return -2;
    FileMutationGuard lookup(false);
    if (int error=VfsResolvePath(path,resolved,size)) return error;
    const char* relative=nullptr;
    MountEntry* mount=FindMount(resolved,&relative);
    if (!mount || !mount->fsOps) return -2;
    int error=0;
    Vnode* opened=mount->fsOps->open_mode_checked ?
        mount->fsOps->open_mode_checked(mount->mountPriv,mount->pdrv,relative,VFS_O_EXEC,0666,&error) :
        mount->fsOps->open_mode ? mount->fsOps->open_mode(mount->mountPriv,mount->pdrv,relative,VFS_O_EXEC,0666) :
        mount->fsOps->open ? mount->fsOps->open(mount->mountPriv,mount->pdrv,relative,VFS_O_EXEC) : nullptr;
    if (!opened) return error?error:-2;
    if (opened->type==VnodeType::Dir) { VfsClose(opened);return -13; }
    *vnode=opened;return 0;
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
    if ((flags & VFS_O_METADATA) &&
        (flags & (VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC | VFS_O_READWRITE | VFS_O_APPEND))) {
        if (error) *error = -22;
        return nullptr;
    }
    FileMutationGuard mutation((flags & (VFS_O_TRUNC | VFS_O_CREATE)) != 0);
    if (!path || !path[0]) return nullptr;

    char resolved[512];
    const bool exclusive=(flags & (VFS_O_CREATE | VFS_O_EXCL))==(VFS_O_CREATE | VFS_O_EXCL);
    PathWalkOptions options{!exclusive, bool(flags & VFS_O_CREATE), false, -21};
    if (int result=VfsResolvePath(path,resolved,sizeof(resolved),options)) {
        if (error) *error=result;
        return nullptr;
    }
    const char* lookup=resolved;
    if (exclusive) {
        VnodeStat st{};
        int result=VfsLstatPathRaw(lookup,&st);
        if (!result || result!=-2) {
            if (error) *error=result?result:-17;
            return nullptr;
        }
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
    if (flags & VFS_O_TRUNC) {
        if (!(openFlags & VFS_O_WRITE)) openFlags |= VFS_O_READWRITE;
        openFlags |= VFS_O_WRITE;
    }
    Vnode* vn = nullptr;
    if (mount->fsOps->open_mode_checked)
        vn = mount->fsOps->open_mode_checked(mount->mountPriv, mount->pdrv,
                                              relPath, openFlags, mode, error);
    else if (mount->fsOps->open_mode)
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
    if (vn && vn->type==VnodeType::Dir) {
        const uint32_t length=StrLen(resolved);
        vn->directoryPath=static_cast<char*>(kmalloc(length+1));
        if(!vn->directoryPath) {
            if(error)*error=-12;
            VfsClose(vn);return nullptr;
        }
        for(uint32_t i=0;i<=length;++i)vn->directoryPath[i]=resolved[i];
    }
    return vn;
}

int VfsDirectoryPath(Vnode* vn,char* out,uint32_t capacity)
{
    if(!vn || vn->type!=VnodeType::Dir)return -20;
    if(!out || !capacity)return -22;
    if(!vn->directoryPath)return -9;
    FileMutationGuard lookup(false);
    const char* path=vn->directoryPath;
    char current[512];
    if(vn->ops && vn->ops->directory_path) {
        const char* ignored=nullptr;
        MountEntry* mount=FindMount(path,&ignored);
        if(!mount)return -2;
        int error=vn->ops->directory_path(vn,current,sizeof(current));
        if(error)return error;
        const uint32_t prefix=StrLen(mount->mountPoint),relative=StrLen(current);
        const bool root=prefix==1 && mount->mountPoint[0]=='/';
        const bool relativeRoot=relative==1 && current[0]=='/';
        const uint32_t length=root?relative:prefix+(relativeRoot?0:relative);
        if(length>=capacity)return -36;
        if(root)StrCopy(out,current,capacity);
        else {
            for(uint32_t i=0;i<prefix;++i)out[i]=mount->mountPoint[i];
            if(!relativeRoot)for(uint32_t i=0;i<relative;++i)out[prefix+i]=current[i];
            out[length]=0;
        }
        return 0;
    }
    if(StrLen(path)>=capacity)return -36;
    StrCopy(out,path,capacity);return 0;
}

int VfsChmod(Vnode* vn, uint16_t mode)
{
    FileMutationGuard mutation;
    if (!vn) return -9;
    if (!vn->ops || !vn->ops->chmod) return -95;
    return vn->ops->chmod(vn, mode);
}

int VfsChmodPath(const char* path, uint16_t mode)
{
    if (!path || !path[0]) return -2;
    FileMutationGuard mutation;
    char resolved[512];
    if (int error=VfsResolvePath(path,resolved,sizeof(resolved),{true})) return error;
    const char* lookup=resolved;
    const char* relPath = nullptr;
    MountEntry* mount = FindMount(lookup, &relPath);
    if (!mount) return -2;
    if (!mount->fsOps || !mount->fsOps->chmod) return -95;
    return mount->fsOps->chmod(mount->mountPriv, mount->pdrv, relPath, mode);
}

int VfsChown(Vnode* vn, uint32_t uid, uint32_t gid)
{
    FileMutationGuard mutation;
    if (!vn) return -9;
    if (!vn->ops || !vn->ops->chown) return -95;
    return vn->ops->chown(vn, uid, gid);
}

int VfsChownPath(const char* path, uint32_t uid, uint32_t gid, bool nofollow)
{
    if (!path || !path[0]) return -2;
    FileMutationGuard mutation;
    char resolved[512];
    if (int error=VfsResolvePath(path,resolved,sizeof(resolved),{!nofollow})) return error;
    const char* lookup=resolved;
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

int VfsAppend(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset)
{
    if (!vn || vn->type != VnodeType::File || !vn->ops->write) return -22;
    if (vn->ops->file_object)
        return FileObjectAppend(vn->ops->file_object(vn), vn, buf, len, offset);
    if (vn->ops->append) return vn->ops->append(vn, buf, len, offset);
    return -95; // A stat followed by write would race other opens/truncation.
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
    if (VfsLstatPathRaw(path, &st)) return false;
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

int VfsSpaceInfo(Vnode* vn, VfsSpace* out)
{
    if (!vn || !out) return -22;
    *out = {};
    if (!vn->ops || !vn->ops->space) return -38; // ENOSYS, never fabricated capacity.
    return vn->ops->space(vn, out);
}

int VfsStatPath(const char* path, VnodeStat* st)
{
    if (!st) return -22;
    *st={};FileMutationGuard lookup(false);
    char resolved[512];
    if (int error=VfsResolvePath(path,resolved,sizeof(resolved))) return error;
    return VfsLstatPathRaw(resolved,st);
}

int VfsLstatPath(const char* path, VnodeStat* st)
{
    if (!st) return -22;
    *st={};FileMutationGuard lookup(false);
    char resolved[512];
    if (int error=VfsResolvePath(path,resolved,sizeof(resolved),{false})) return error;
    return VfsLstatPathRaw(resolved,st);
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
    FileMutationGuard mutation;
    char resolved[512];
    if (int error=VfsResolvePath(path,resolved,sizeof(resolved),{!nofollow})) return error;
    const char* lookup=resolved;
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

int VfsUnlink(const char* path) { return VfsRemovePath(path,false); }

int VfsRemovePath(const char* path, bool directory)
{
    FileMutationGuard mutation;
    char resolved[512];
    if (int error=VfsResolvePath(path,resolved,sizeof(resolved),{false,false,true})) return error;
    if (VfsTrailingSlash(resolved)) {
        VnodeStat st{};
        if (int error=VfsLstatPathRaw(resolved,&st)) return error;
        if (!st.isDir || st.isSymlink) return -20;
    }
    if (VfsMappedPathBusy(resolved)) return -16;
    const char* relative=nullptr;MountEntry* mount=FindMount(resolved,&relative);
    if (!mount || !mount->fsOps) return -2;
    if (mount->fsOps->remove_path) return mount->fsOps->remove_path(mount->mountPriv,mount->pdrv,relative,directory);
    VnodeStat st{};
    if (int error=VfsLstatPathRaw(resolved,&st)) return error;
    if (st.isDir!=directory) return directory?-20:-21;
    return mount->fsOps->unlink ? mount->fsOps->unlink(mount->mountPriv,mount->pdrv,relative) : -95;
}

int VfsMkdir(const char* path,uint16_t mode)
{
    FileMutationGuard mutation;
    char resolved[512];
    if (int error=VfsResolvePath(path,resolved,sizeof(resolved),{false,true,true})) return error;
    const char* relative=nullptr;MountEntry* mount=FindMount(resolved,&relative);
    if (!mount || !mount->fsOps) return -2;
    return mount->fsOps->mkdir ? mount->fsOps->mkdir(mount->mountPriv,mount->pdrv,relative,mode) : -95;
}

Vnode* VfsCreateSocket(const char* path, uint16_t mode, int* error)
{
    FileMutationGuard mutation;
    int localError = 0;
    if (!error) error = &localError;
    *error = 0;
    char resolved[512];
    if ((*error = VfsResolvePath(path, resolved, sizeof(resolved), {false, true, true})))
        return nullptr;
    VnodeStat existing{};
    int result = VfsLstatPathRaw(resolved, &existing);
    if (result != -2) { *error = result ? result : -98; return nullptr; }
    const char* relative = nullptr;
    MountEntry* mount = FindMount(resolved, &relative);
    if (!mount || !mount->fsOps) { *error = -2; return nullptr; }
    if (!mount->fsOps->create_socket) { *error = -95; return nullptr; }
    Vnode* node = mount->fsOps->create_socket(mount->mountPriv, mount->pdrv,
                                             relative, mode, error);
    if (*error == -17) *error = -98;
    if (!node && !*error) *error = -5;
    return node;
}

int VfsRename(const char* oldPath,const char* newPath)
{
    FileMutationGuard mutation;
    char oldResolved[512],newResolved[512];
    if (int error=VfsResolvePath(oldPath,oldResolved,sizeof(oldResolved),{false,false,true})) return error;
    if (int error=VfsResolvePath(newPath,newResolved,sizeof(newResolved),{false,true,true})) return error;
    if (VfsTrailingSlash(oldResolved) || VfsTrailingSlash(newResolved)) {
        VnodeStat st{};
        if (int error=VfsLstatPathRaw(oldResolved,&st)) return error;
        if (!st.isDir || st.isSymlink) return -20;
        int error=VfsLstatPathRaw(newResolved,&st);
        if (error && error!=-2) return error;
        if (!error && (!st.isDir || st.isSymlink)) return -20;
    }
    if (VfsMappedPathBusy(oldResolved) || VfsMappedPathBusy(newResolved)) return -16;
    const char* oldRelative=nullptr;const char* newRelative=nullptr;
    MountEntry* oldMount=FindMount(oldResolved,&oldRelative);MountEntry* newMount=FindMount(newResolved,&newRelative);
    if (!oldMount || !newMount || !oldMount->fsOps) return -2;
    if (oldMount!=newMount) return -18;
    return oldMount->fsOps->rename ? oldMount->fsOps->rename(oldMount->mountPriv,oldMount->pdrv,oldRelative,newRelative) : -95;
}

int VfsSymlink(const char* target,const char* linkPath)
{
    if (!target || !target[0]) return -22;
    FileMutationGuard mutation;
    char resolved[512];
    if (int error=VfsResolvePath(linkPath,resolved,sizeof(resolved),{false,true,true})) return error;
    if (VfsTrailingSlash(resolved)) {
        VnodeStat st{};
        int error=VfsLstatPathRaw(resolved,&st);
        return error?error:-17;
    }
    const char* relative=nullptr;MountEntry* mount=FindMount(resolved,&relative);
    if (!mount || !mount->fsOps) return -2;
    return mount->fsOps->symlink ? mount->fsOps->symlink(mount->mountPriv,mount->pdrv,target,relative) : -95;
}

int VfsReadlink(const char* path,char* buf,uint64_t bufsiz)
{
    if (!buf || !bufsiz) return -22;
    FileMutationGuard lookup(false);
    char resolved[512];
    if (int error=VfsResolvePath(path,resolved,sizeof(resolved),{false})) return error;
    return VfsReadlinkRaw(resolved,buf,bufsiz);
}

void VfsClose(Vnode* vn)
{
    if (!vn) return;
    if (vn->ops && vn->ops->close) vn->ops->close(vn);
    kfree(vn->directoryPath);
    kfree(vn);
}

} // namespace brook
