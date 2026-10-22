#include "vfs.h"
#include "vfs_object.h"
#include "vfs_path.h"
#include "retained_path_walk.h"
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
#ifndef BROOK_HOST_TEST
#include "module.h"
#else
namespace brook { bool ModuleRetainAddresses(const uintptr_t*,uint32_t) __attribute__((weak)); }
#endif

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

static bool VfsPinProvider(const uintptr_t* addresses,uint32_t count)
{
#ifdef BROOK_HOST_TEST
    if(!ModuleRetainAddresses)return true; // hosted backends have process lifetime
#endif
    return ModuleRetainAddresses(addresses,count);
}

bool VfsRegisterFs(const char* name, const VfsFsOps* ops)
{
    if (!name || !ops) return false;
    FileMutationGuard mutation;
    // Pin the table before reading it, then pin every callback/name owner.
    // Retention is permanent, as for scheduler providers; unregistering a name
    // cannot unload callbacks still referenced by backing or backend objects.
    const uintptr_t table=reinterpret_cast<uintptr_t>(ops);
    if(!VfsPinProvider(&table,1))return false;
    uintptr_t addresses[25];uint32_t count=0;
    addresses[count++]=reinterpret_cast<uintptr_t>(name);
#define VFS_PIN_CALLBACK(field) if(ops->field)addresses[count++]=reinterpret_cast<uintptr_t>(ops->field)
    VFS_PIN_CALLBACK(mount);VFS_PIN_CALLBACK(unmount);VFS_PIN_CALLBACK(open);
    VFS_PIN_CALLBACK(stat_path);VFS_PIN_CALLBACK(lstat_path);VFS_PIN_CALLBACK(unlink);
    VFS_PIN_CALLBACK(mkdir);VFS_PIN_CALLBACK(rename);VFS_PIN_CALLBACK(symlink);
    VFS_PIN_CALLBACK(readlink);VFS_PIN_CALLBACK(sync);VFS_PIN_CALLBACK(set_times);
    VFS_PIN_CALLBACK(open_mode);VFS_PIN_CALLBACK(chmod);VFS_PIN_CALLBACK(remove_path);
    VFS_PIN_CALLBACK(can_unmount);VFS_PIN_CALLBACK(chown);VFS_PIN_CALLBACK(open_mode_checked);
    VFS_PIN_CALLBACK(check_access);VFS_PIN_CALLBACK(check_access_real);VFS_PIN_CALLBACK(create_socket);
    VFS_PIN_CALLBACK(root_object);VFS_PIN_CALLBACK(vnode_object);
#undef VFS_PIN_CALLBACK
    if(!VfsPinProvider(addresses,count) || !ops->mount)return false;
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
    FileMutationGuard mutation;
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

// Traversal safety bound, independent of the number of allocated attachments.
static constexpr uint32_t VFS_MAX_MOUNT_DEPTH = 4096;

// A driver mount is backing, not a namespace attachment. Copies and binds
// retain this object instead of calling the driver's mount callback again.
struct MountBacking {
    const VfsFsOps* fsOps;
    void* mountPriv;
    uint8_t pdrv;
    uint64_t references;
    MountBacking* next;
    void RetainLocked() {
        if(!references || references==~uint64_t{0})__builtin_trap();
        ++references;
    }
};
static MountBacking* g_backings;

struct MountEntry {
    MountEntry* next;
    char             mountPoint[64];
    char             fsType[16];     // filesystem type name (e.g. "fatfs", "ext2")
    MountBacking*    backing;     // owned driver-mount reference
    bool             used;
    uint64_t         vnodeHolds;
    uint64_t         pathHolds;
    uint64_t         publication;
    VfsPath          covered;
    uint64_t         rootIdentity;
    const VfsObjectOps* rootObjectOps;
    bool             rootIdentityKnown;
    bool             linked;
};

// Membership is protected by the sleeping VFS transaction. Short metadata
// readers (including procfs callbacks already in that transaction) use viewLock
// so they never recursively acquire the sleeping lock or observe freed nodes.
static IrqSpinLock g_mountViewLock;
struct MountRegistry {
    MountEntry* head=nullptr;
    uint32_t count=0;
    struct Iterator {
        MountEntry* value;
        MountEntry& operator*() const { return *value; }
        Iterator& operator++() { value=value->next;return *this; }
        bool operator!=(Iterator other) const { return value!=other.value; }
    };
    Iterator begin() const { return {head}; }
    Iterator end() const { return {nullptr}; }
    MountEntry* At(uint32_t index) const {
        auto* node=head;while(node && index--)node=node->next;return node;
    }
    void Publish(MountEntry* node) {
        auto flags=IrqSpinLockAcquire(&g_mountViewLock);
        node->next=head;head=node;++count;
        IrqSpinLockRelease(&g_mountViewLock,flags);
    }
    void Remove(MountEntry* node) {
        auto flags=IrqSpinLockAcquire(&g_mountViewLock);
        auto** entry=&head;while(*entry && *entry!=node)entry=&(*entry)->next;
        if(!*entry || !count)__builtin_trap();
        *entry=node->next;node->next=nullptr;node->used=false;--count;
        IrqSpinLockRelease(&g_mountViewLock,flags);
    }
};
static MountRegistry g_mounts;
struct MountViewGuard {
    uint64_t flags;
    MountViewGuard():flags(IrqSpinLockAcquire(&g_mountViewLock)) {}
    ~MountViewGuard() { IrqSpinLockRelease(&g_mountViewLock,flags); }
};
static MountEntry* g_rootMount;
static uint64_t g_mountPublication;

// All backing registry/reference changes occur in a sleeping VFS mutation
// transaction. Zero-reference backings remain registered if final teardown
// fails; global sync reaches them and retries retirement without losing data.
static int VfsBackingUnmountLocked(MountBacking* backing)
{
    if(backing->fsOps->can_unmount && !backing->fsOps->can_unmount(backing->mountPriv))return -16;
    int error=backing->fsOps->unmount?backing->fsOps->unmount(backing->mountPriv):0;
    return error>0?-5:error;
}
static void VfsBackingFreeLocked(MountBacking* backing)
{
    auto** link=&g_backings;
    while(*link && *link!=backing)link=&(*link)->next;
    if(!*link || backing->references)__builtin_trap();
    *link=backing->next;kfree(backing);
}
static int VfsBackingDropLocked(MountBacking* backing,bool deferFailure=false)
{
    if(!backing || !backing->references)__builtin_trap();
    if(backing->references>1) { --backing->references;return 0; }
    int error=VfsBackingUnmountLocked(backing);
    // Explicit unmount keeps its attachment and reference on failure. Private
    // construction rollback instead leaves an unreachable registry entry for
    // checked sync to flush/retry, with no borrowed stack or attachment pointer.
    if(error && !deferFailure)return error;
    backing->references=0;
    if(!error)VfsBackingFreeLocked(backing);
    return error;
}

// Called under the namespace lock before a newly opened vnode is published.
// Final close drops the hold only after the backend has finished using it.
static void VfsAttachMount(Vnode* vnode, MountEntry* mount)
{
    vnode->mountOwner=mount;
    __atomic_fetch_add(&mount->vnodeHolds,uint64_t{1},__ATOMIC_RELAXED);
}

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

struct VfsRenameLabels {
    struct Change { Change* next;MountEntry* mount;char label[64]; };
    Change* head=nullptr;
    ~VfsRenameLabels() { while(head) { auto* next=head->next;kfree(head);head=next; } }
    int Add(MountEntry* mount,const char* prefix,const char* suffix) {
        auto* change=static_cast<Change*>(kmalloc(sizeof(Change)));
        if(!change)return -12;
        change->mount=mount;StrCopy(change->label,prefix,sizeof(change->label));
        const uint32_t length=StrLen(prefix);
        StrCopy(change->label+length,suffix,sizeof(change->label)-length);
        change->next=head;head=change;return 0;
    }
    void Apply() {
        MountViewGuard held;
        for(auto* change=head;change;change=change->next)
            StrCopy(change->mount->mountPoint,change->label,sizeof(change->mount->mountPoint));
    }
};

// Find the best (longest-prefix) mount for a given absolute path.
// Returns the mount entry and sets *relPath to the path within the mount.
static MountEntry* FindMount(const char* absPath, const char** relPath)
{
    MountEntry* best      = nullptr;
    uint32_t    bestLen   = 0;
    if(g_rootMount && g_rootMount->rootIdentityKnown) {
        // A longest-prefix scan can resurrect descendants hidden by an
        // overmount. Follow visible attachment edges from the current root.
        best=g_rootMount;
        for(uint32_t step=0;step<VFS_MAX_MOUNT_DEPTH;++step) {
            MountEntry* next=nullptr;uint32_t nextLength=~uint32_t{0};
            for(auto& mount:g_mounts) {
                if(!mount.used || !mount.linked || mount.covered.attachment!=best)continue;
                const uint32_t length=StrLen(mount.mountPoint);bool match=true;
                for(uint32_t i=0;i<length;++i)if(absPath[i]!=mount.mountPoint[i]) { match=false;break; }
                if(!match || (absPath[length] && absPath[length]!='/'))continue;
                if(length<nextLength) { next=&mount;nextLength=length; }
            }
            if(!next)break;
            best=next;
        }
        bestLen=StrLen(best->mountPoint);
        if(relPath) { const char* rem=absPath+bestLen;*relPath=*rem?rem:"/"; }
        return best;
    }

    for(auto& candidate:g_mounts)
    {
        if (!candidate.used) continue;
        uint32_t mpLen = StrLen(candidate.mountPoint);
        bool match = true;
        for (uint32_t j = 0; j < mpLen; ++j) {
            if (absPath[j] != candidate.mountPoint[j]) { match = false; break; }
        }
        if (!match) continue;
        if (mpLen > 1 && absPath[mpLen] != '\0' && absPath[mpLen] != '/') continue;
        if (mpLen > bestLen || (mpLen==bestLen && best && candidate.publication>best->publication)) { bestLen = mpLen; best = &candidate; }
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
    if (!mount || !mount->backing->fsOps) return -2;

    int result = -2;
    if (mount->backing->fsOps->lstat_path)
        result = mount->backing->fsOps->lstat_path(mount->backing->mountPriv, mount->backing->pdrv, relPath, st);
    else if (mount->backing->fsOps->stat_path)
        result = mount->backing->fsOps->stat_path(mount->backing->mountPriv, mount->backing->pdrv, relPath, st);
    // Legacy stat callbacks use -1 for missing entries, not authorization.
    if (result == -1) result = -2;
    if (result == -2) {
        // Mounts can have virtual ancestors (the boot probe uses /mnt/probe
        // without a physical /mnt). Do not fabricate ordinary missing paths or
        // replace a backing error/file: only an absent ancestor of a live mount.
        uint32_t length = StrLen(path);
        for (const auto& candidate : g_mounts) {
            if (!candidate.used || StrLen(candidate.mountPoint) <= length) continue;
            if(g_rootMount && g_rootMount->rootIdentityKnown && FindMount(candidate.mountPoint,nullptr)!=&candidate)continue;
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
    if (!mount || !mount->backing->fsOps) return -2;
    // A filesystem root is a directory, including when reached through a
    // mountpoint like /data. Backend parent/name lookup cannot represent it.
    if (relPath[0]=='/' && !relPath[1]) return -22;
    if (!mount->backing->fsOps->readlink) {
        VnodeStat stat{};
        const int result=VfsLstatPathRaw(path,&stat);
        return result ? (result==-1?-2:result) : -22;
    }
    return mount->backing->fsOps->readlink(mount->backing->mountPriv, mount->backing->pdrv, relPath, buf, bufsiz);
}

static int VfsCheckAccessRaw(const char* path, const VnodeStat& st, uint8_t required, bool realIds = false)
{
    MountEntry* mount = FindMount(path, nullptr);
    if (!mount || !mount->backing->fsOps) return -2;
    auto policy = realIds ? mount->backing->fsOps->check_access_real : mount->backing->fsOps->check_access;
    if (policy) return policy(mount->backing->mountPriv, &st, required);
    // Never substitute effective-ID authorization for a real-ID inquiry.
    if (realIds && mount->backing->fsOps->check_access) return -95;
    return 0; // Backends without a policy retain their explicit legacy behavior.
}
struct VfsPathProvider {
    bool realIds;
    int Stat(const char* p, VnodeStat& st) {
        int error=VfsLstatPathRaw(p,&st);
        MountEntry* mount=FindMount(p,nullptr);
        // The walk cannot turn procfs FD objects into textual symlink targets.
        // Only its internal node is opaque; final stat/lstat keep backend data.
        if (!error && mount && mount->backing->fsOps->opaque_symlinks) st.isSymlink=false;
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

// Object traversal shares the namespace transaction used by legacy mutations.
#include "vfs_retained.inc"

// ---- Public API ----

void VfsInit()
{
    KRwLockInit(&g_fileMutationLock);
    g_rootMount=nullptr;
    g_backings=nullptr;
    g_mountPublication=0;
    g_mountViewLock={};g_mounts={};
    for (uint32_t i = 0; i < VFS_MAX_FS_DRIVERS; ++i) g_fsDrivers[i].used = false;
    SerialPuts("VFS: initialised\n");
}

bool VfsMount(const char* mountPoint, const char* fsName, uint8_t pdrv)
{
    FileMutationGuard mutation;
    if (!mountPoint || mountPoint[0]!='/' || !fsName ||
        StrLen(mountPoint)>=sizeof(MountEntry::mountPoint)) return false;
    const bool isRoot=StrEq(mountPoint,"/");
    // Root replacement and unresolved object attachments are deliberately not
    // supported. Production boot installs the root before any child mounts.
    if(isRoot && g_rootMount)return false;
    if(isRoot)for(const auto& mount:g_mounts)if(mount.used)return false;

    const VfsFsOps* ops = FindFsDriver(fsName);
    if (!ops)
    {
        SerialPrintf("VFS: unknown filesystem '%s'\n", fsName);
        return false;
    }

    if(g_mounts.count==~uint32_t{0})return false;
    auto* slot=static_cast<MountEntry*>(kmalloc(sizeof(MountEntry)));
    if(!slot)return false;
    *slot={};
    MountBacking* backing=nullptr;
    for(auto* candidate=g_backings;candidate;candidate=candidate->next)
        if(candidate->fsOps==ops && candidate->pdrv==pdrv) { backing=candidate;break; }
    if(backing) {
        // FAT's drive slots are global to its driver. Do not remount over a
        // failed retirement: its later unmount could unregister the new mount.
        if(!backing->references) { kfree(slot);return false; }
        backing->RetainLocked();
    } else {
        backing=static_cast<MountBacking*>(kmalloc(sizeof(MountBacking)));
        if(!backing) { kfree(slot);return false; }
        *backing={};backing->fsOps=ops;backing->pdrv=pdrv;backing->references=1;
        void* privateData=nullptr;
        if(!ops->mount(pdrv,&privateData)) {
            SerialPrintf("VFS: mount failed for '%s' at '%s'\n",fsName,mountPoint);
            kfree(backing);kfree(slot);return false;
        }
        backing->mountPriv=privateData;backing->next=g_backings;g_backings=backing;
    }
    void* priv=backing->mountPriv;
    *slot={};slot->backing=backing;
    StrCopy(slot->mountPoint,mountPoint,sizeof(slot->mountPoint));
    StrCopy(slot->fsType,fsName,sizeof(slot->fsType));
    int error=0;
    if(ops->root_object) {
        VfsObject* object=nullptr;
        error=ops->root_object(priv,pdrv,&object);
        if(!error && (!object || !object->ops || !object->ops->stat))error=-5;
        VnodeStat stat{};
        if(!error)error=object->ops->stat(object,&stat);
        if(!error && !stat.isDir)error=-20;
        if(!error) { slot->rootIdentity=object->identity;slot->rootObjectOps=object->ops;slot->rootIdentityKnown=true; }
        VfsObjectRelease(object);
    }
    if(!error && !isRoot && g_rootMount && g_rootMount->rootIdentityKnown)
        error=VfsPrepareMountpointLocked(mountPoint,&slot->covered);
    if(error) {
        VfsPathRelease(slot->covered);*slot={};
        // Rollback is private: no path or vnode has observed this mount.
        VfsBackingDropLocked(backing,true);
        kfree(slot);return false;
    }
    slot->linked=isRoot || slot->covered.object;
    slot->publication=++g_mountPublication;
    slot->used=true;
    g_mounts.Publish(slot);
    if(isRoot)g_rootMount=slot;

    DbgPrintf("VFS: mounted '%s' at '%s' (pdrv=%u)\n", fsName, mountPoint, pdrv);
    return true;
}

bool VfsUnmount(const char* mountPoint)
{
    VfsPath covered{};
    {
        FileMutationGuard mutation;
        if(!mountPoint)return false;
        MountEntry* selected=nullptr;
        // Stacked attachments are removed top-first, identified by their edge
        // rather than mount-table slot ordering.
        for(auto& mount:g_mounts) {
            if(!mount.used || !StrEq(mount.mountPoint,mountPoint))continue;
            bool coveredByChild=false;
            for(const auto& child:g_mounts)
                if(child.used && child.covered.attachment==&mount)coveredByChild=true;
            if(!coveredByChild)selected=&mount;
        }
        if(!selected)return false;
        auto& mount=*selected;
        if(__atomic_load_n(&mount.vnodeHolds,__ATOMIC_ACQUIRE) ||
           __atomic_load_n(&mount.pathHolds,__ATOMIC_ACQUIRE))return false;
        if(VfsBackingDropLocked(mount.backing))return false;
        mount.backing=nullptr;
        covered=mount.covered;mount.covered={};
        if(g_rootMount==selected)g_rootMount=nullptr;
        g_mounts.Remove(selected);kfree(selected);
    }
    // Namespace edge is unpublished before its parent/object holds are dropped.
    VfsPathRelease(covered);
    SerialPrintf("VFS: unmounted '%s'\n",mountPoint);
    return true;
}

uint32_t VfsRootMountCount()
{
    MountViewGuard held;
    uint32_t count = 0;
    for(const auto& mount:g_mounts)
    {
        if (!mount.used) continue;
        const char* mp = mount.mountPoint;
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
    MountViewGuard held;
    if (!out || outSize == 0) return false;

    uint32_t seen = 0;
    for(const auto& mount:g_mounts)
    {
        if (!mount.used) continue;
        const char* mp = mount.mountPoint;
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

uint32_t VfsMountMaxSlots() { MountViewGuard held;return g_mounts.count; }

VfsMountSnapshot VfsMountSnapshotAt(uint32_t idx)
{
    VfsMountSnapshot snap = {};
    MountViewGuard held;auto* entry=g_mounts.At(idx);
    if (!entry) return snap;
    const auto& m = *entry;
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
    if (!mount || !mount->backing->fsOps) return -2;
    int error=0;
    Vnode* opened=mount->backing->fsOps->open_mode_checked ?
        mount->backing->fsOps->open_mode_checked(mount->backing->mountPriv,mount->backing->pdrv,relative,VFS_O_EXEC,0666,&error) :
        mount->backing->fsOps->open_mode ? mount->backing->fsOps->open_mode(mount->backing->mountPriv,mount->backing->pdrv,relative,VFS_O_EXEC,0666) :
        mount->backing->fsOps->open ? mount->backing->fsOps->open(mount->backing->mountPriv,mount->backing->pdrv,relative,VFS_O_EXEC) : nullptr;
    if (!opened) return error?error:-2;
    VfsAttachMount(opened,mount);
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
    if(flags & VFS_O_CREATE)if(int result=VfsReservedMutationLocked(path)) {
        if(error)*error=result;return nullptr;
    }
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

    if (!mount->backing->fsOps) return nullptr;
    // O_TRUNC must use the same resize gate and alias invalidation as truncate.
    int openFlags = flags & ~(VFS_O_TRUNC | VFS_O_EXCL);
    if (flags & VFS_O_TRUNC) {
        if (!(openFlags & VFS_O_WRITE)) openFlags |= VFS_O_READWRITE;
        openFlags |= VFS_O_WRITE;
    }
    Vnode* vn = nullptr;
    if (mount->backing->fsOps->open_mode_checked)
        vn = mount->backing->fsOps->open_mode_checked(mount->backing->mountPriv, mount->backing->pdrv,
                                              relPath, openFlags, mode, error);
    else if (mount->backing->fsOps->open_mode)
        vn = mount->backing->fsOps->open_mode(mount->backing->mountPriv, mount->backing->pdrv, relPath, openFlags, mode);
    else if (mount->backing->fsOps->open)
        vn = mount->backing->fsOps->open(mount->backing->mountPriv, mount->backing->pdrv, relPath, openFlags);
    if (vn) VfsAttachMount(vn,mount);
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
    FileMutationGuard lookup(false);
    auto* actual=static_cast<MountEntry*>(vn->mountOwner);
    if(actual && (actual->backing->fsOps->vnode_object ||
                  (vn->backendObject && vn->backendObject->ops==&g_virtualDirectoryOps))) {
        VfsPath retained{};int error=VfsPathFromVnodeLocked(vn,&retained);
        if(!error)error=VfsGlobalDirectoryLocked(retained,out,capacity);
        VfsPathRelease(retained);return error;
    }
    if(!vn->directoryPath)return -9;
    const char* path=vn->directoryPath;
    char current[512];
    if(vn->ops && vn->ops->directory_path) {
        // The opening name can now resolve through a different overmount.
        // The retained mount, not that name, owns this backend directory.
        auto* mount=static_cast<MountEntry*>(vn->mountOwner);
        if(!mount || !mount->used)return -9;
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
    if (!mount->backing->fsOps || !mount->backing->fsOps->chmod) return -95;
    return mount->backing->fsOps->chmod(mount->backing->mountPriv, mount->backing->pdrv, relPath, mode);
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
    if (!mount->backing->fsOps || !mount->backing->fsOps->chown) return -95;
    return mount->backing->fsOps->chown(mount->backing->mountPriv, mount->backing->pdrv, relPath, uid, gid, nofollow);
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

static bool VfsMappedMutationBusy(const VnodeStat& stat)
{
    // Match the established mutation policy: Ext2 open-inode holders preserve
    // orphaned/moved backing; FAT must protect the physical directory entry.
    if(((stat.dev>>8)&0xfff)!=253)return false;
    return g_mappingPins.Busy(stat.dev,stat.ino,stat.isDir);
}
static bool VfsMappedPathBusy(const char* path)
{
    VnodeStat stat{};return !VfsLstatPathRaw(path,&stat) && VfsMappedMutationBusy(stat);
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
    if (!mount || !mount->backing->fsOps) return -2;
    if (!mount->backing->fsOps->set_times) return -95;
    return mount->backing->fsOps->set_times(mount->backing->mountPriv, mount->backing->pdrv, relPath, times, nofollow);
}

int VfsSync(Vnode* vn)
{
    if (vn) return VfsFsync(vn);
    // Try every mount even after a failure, but never acknowledge a failed
    // filesystem/device flush as successful to sync(2) or the VM bridge.
    int result = FileObjectWritebackAll();
    FileMutationGuard mutation;
    for(auto* backing=g_backings;backing;) {
        auto* next=backing->next;
        if(backing->fsOps->sync) {
            int error=backing->fsOps->sync(backing->mountPriv);
            if(!result && error)result=error<0?error:-5;
        }
        if(!backing->references) {
            int error=VfsBackingUnmountLocked(backing);
            if(!error)VfsBackingFreeLocked(backing);
            else if(!result)result=error;
        }
        backing=next;
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
    if(int error=VfsReservedMutationLocked(path,directory))return error;
    char resolved[512];
    if (int error=VfsResolvePath(path,resolved,sizeof(resolved),{false,false,true})) return error;
    if (VfsTrailingSlash(resolved)) {
        VnodeStat st{};
        if (int error=VfsLstatPathRaw(resolved,&st)) return error;
        if (!st.isDir || st.isSymlink) return -20;
    }
    if (VfsMappedPathBusy(resolved)) return -16;
    const char* relative=nullptr;MountEntry* mount=FindMount(resolved,&relative);
    if (!mount || !mount->backing->fsOps) return -2;
    if (mount->backing->fsOps->remove_path) return mount->backing->fsOps->remove_path(mount->backing->mountPriv,mount->backing->pdrv,relative,directory);
    VnodeStat st{};
    if (int error=VfsLstatPathRaw(resolved,&st)) return error;
    if (st.isDir!=directory) return directory?-20:-21;
    return mount->backing->fsOps->unlink ? mount->backing->fsOps->unlink(mount->backing->mountPriv,mount->backing->pdrv,relative) : -95;
}

int VfsMkdir(const char* path,uint16_t mode)
{
    FileMutationGuard mutation;
    if(int error=VfsReservedMutationLocked(path))return error;
    char resolved[512];
    if (int error=VfsResolvePath(path,resolved,sizeof(resolved),{false,true,true})) return error;
    const char* relative=nullptr;MountEntry* mount=FindMount(resolved,&relative);
    if (!mount || !mount->backing->fsOps) return -2;
    return mount->backing->fsOps->mkdir ? mount->backing->fsOps->mkdir(mount->backing->mountPriv,mount->backing->pdrv,relative,mode) : -95;
}

Vnode* VfsCreateSocket(const char* path, uint16_t mode, int* error)
{
    FileMutationGuard mutation;
    int localError = 0;
    if (!error) error = &localError;
    *error = 0;
    if((*error=VfsReservedMutationLocked(path)))return nullptr;
    char resolved[512];
    if ((*error = VfsResolvePath(path, resolved, sizeof(resolved), {false, true, true})))
        return nullptr;
    VnodeStat existing{};
    int result = VfsLstatPathRaw(resolved, &existing);
    if (result != -2) { *error = result ? result : -98; return nullptr; }
    const char* relative = nullptr;
    MountEntry* mount = FindMount(resolved, &relative);
    if (!mount || !mount->backing->fsOps) { *error = -2; return nullptr; }
    if (!mount->backing->fsOps->create_socket) { *error = -95; return nullptr; }
    Vnode* node = mount->backing->fsOps->create_socket(mount->backing->mountPriv, mount->backing->pdrv,
                                             relative, mode, error);
    if (node) VfsAttachMount(node,mount);
    if (*error == -17) *error = -98;
    if (!node && !*error) *error = -5;
    return node;
}

int VfsRename(const char* oldPath,const char* newPath)
{
    FileMutationGuard mutation;
    if(int error=VfsReservedMutationLocked(oldPath))return error;
    if(int error=VfsReservedMutationLocked(newPath))return error;
    char oldResolved[512],newResolved[512];
    if (int error=VfsResolvePath(oldPath,oldResolved,sizeof(oldResolved),{false,false,true})) return error;
    if (int error=VfsResolvePath(newPath,newResolved,sizeof(newResolved),{false,true,true})) return error;
    if(!StrEq(oldResolved,newResolved))if(int error=VfsReservedMutationLocked(newResolved,true))return error;
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
    if (!oldMount || !newMount || !oldMount->backing->fsOps) return -2;
    if (oldMount!=newMount) return -18;
    const uint32_t oldLength=StrLen(oldResolved),newLength=StrLen(newResolved);
    auto renamedAncestor=[&](const MountEntry& mount) {
        if(!mount.used || StrLen(mount.mountPoint)<=oldLength)return false;
        // An upper attachment rename must not relabel a hidden lower tree
        // whose opening labels happen to share the same textual prefix.
        const MountEntry* ancestor=&mount;
        for(uint32_t step=0;ancestor && ancestor!=oldMount && step<VFS_MAX_MOUNT_DEPTH;++step)
            ancestor=static_cast<MountEntry*>(ancestor->covered.attachment);
        if(ancestor!=oldMount)return false;
        for(uint32_t i=0;i<oldLength;++i)if(mount.mountPoint[i]!=oldResolved[i])return false;
        return mount.mountPoint[oldLength]=='/';
    };
    VfsRenameLabels labels;
    for(auto& mount:g_mounts)if(renamedAncestor(mount)) {
        if(newLength+StrLen(mount.mountPoint+oldLength)>=sizeof(mount.mountPoint))return -36;
        if(int allocationError=labels.Add(&mount,newResolved,mount.mountPoint+oldLength))return allocationError;
    }
    const int error=oldMount->backing->fsOps->rename ? oldMount->backing->fsOps->rename(oldMount->backing->mountPriv,oldMount->backing->pdrv,oldRelative,newRelative) : -95;
    if(!error)labels.Apply();
    return error;
}

int VfsSymlink(const char* target,const char* linkPath)
{
    if (!target || !target[0]) return -22;
    FileMutationGuard mutation;
    if(int error=VfsReservedMutationLocked(linkPath))return error;
    char resolved[512];
    if (int error=VfsResolvePath(linkPath,resolved,sizeof(resolved),{false,true,true})) return error;
    if (VfsTrailingSlash(resolved)) {
        VnodeStat st{};
        int error=VfsLstatPathRaw(resolved,&st);
        return error?error:-17;
    }
    const char* relative=nullptr;MountEntry* mount=FindMount(resolved,&relative);
    if (!mount || !mount->backing->fsOps) return -2;
    return mount->backing->fsOps->symlink ? mount->backing->fsOps->symlink(mount->backing->mountPriv,mount->backing->pdrv,target,relative) : -95;
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
    auto* mount=static_cast<MountEntry*>(vn->mountOwner);
    VfsObject* object=vn->backendObject;
    if (vn->ops && vn->ops->close) vn->ops->close(vn);
    kfree(vn->directoryPath);
    kfree(vn);
    VfsObjectRelease(object);
    if (mount) __atomic_fetch_sub(&mount->vnodeHolds,uint64_t{1},__ATOMIC_RELEASE);
}

void VfsRelease(Vnode* vn)
{
    if (vn && __atomic_fetch_sub(&vn->refCount,uint32_t{1},__ATOMIC_ACQ_REL)==1)
        VfsClose(vn);
}

#include "vfs_retained_ops.inc"

} // namespace brook
