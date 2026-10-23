#include "tmpfs.h"

#include "memory/file_object.h"
#include "memory/heap.h"
#include "process.h"
#include "process_user_namespace.h"
#include "rtc.h"
#include "spinlock.h"
#include "sync/kmutex.h"
#include "vfs.h"
#include "vfs_object.h"
#if defined(BROOK_HOST_TEST)
#include <cstring>
#else
#include "string.h"
#endif

namespace brook
{
namespace
{
uint64_t GlobalBytes = 256ULL * 1024 * 1024;
constexpr uint64_t InstanceInodes = 1024, GlobalInodes = 16384, GlobalMounts = 128;
constexpr uint16_t Regular = 0100000, Directory = 0040000, Link = 0120000, Fifo = 0010000, TypeMask = 0170000;
struct Node;
struct Entry
{
    Entry *next;
    Node *node;
    char name[256];
};
struct Page
{
    Page *next;
    uint64_t index;
    uint8_t bytes[4096];
};
IrqSpinLock budgetLock;
TmpfsUsage globalUsage{};
uint64_t nextDevice = 1;
struct Allocation
{
    uint64_t size;
};
} // namespace
struct Tmpfs
{
    KMutex mutex{};
    UserNamespace *owner = nullptr;
    Node *root = nullptr;
    uint64_t device = 0, nextInode = 1, bytes = 0, inodes = 0;
    uint64_t flags = 0;
    uint64_t byteLimit = TmpfsDefaultBytes;
};
namespace
{
struct Node
{
    Tmpfs *fs;
    uint32_t references;
    uint32_t uid, gid, nlink;
    uint16_t mode;
    bool linked;
    uint64_t ino, size;
    VnodeTime atime, mtime, ctime;
    Node *parent;
    Entry *entries;
    Page *pages;
    char *target;
    char name[256];
    FileObject file;
};
int PrepareBackingPage(void* context,uint64_t index);
struct Guard
{
    Tmpfs *fs;
    explicit Guard(Tmpfs *f) : fs(f)
    {
        KMutexLock(&fs->mutex);
    }
    ~Guard()
    {
        KMutexUnlock(&fs->mutex);
    }
};
VnodeTime Now()
{
    return {static_cast<int64_t>(RtcNow()), 0};
}
bool Reserve(Tmpfs *fs, uint64_t bytes)
{
    auto irq = IrqSpinLockAcquire(&budgetLock);
    bool ok = bytes <= fs->byteLimit - fs->bytes && bytes <= GlobalBytes - globalUsage.bytes;
    if (ok)
    {
        fs->bytes += bytes;
        globalUsage.bytes += bytes;
    }
    IrqSpinLockRelease(&budgetLock, irq);
    return ok;
}
void Unreserve(Tmpfs *fs, uint64_t bytes)
{
    auto irq = IrqSpinLockAcquire(&budgetLock);
    if (bytes > fs->bytes || bytes > globalUsage.bytes)
        __builtin_trap();
    fs->bytes -= bytes;
    globalUsage.bytes -= bytes;
    IrqSpinLockRelease(&budgetLock, irq);
}
void *Allocate(Tmpfs *fs, uint64_t bytes)
{
    if (bytes > fs->byteLimit - sizeof(Allocation))
        return nullptr;
    const uint64_t charge = bytes + sizeof(Allocation);
    auto irq = IrqSpinLockAcquire(&budgetLock);
    bool ok = charge <= fs->byteLimit - fs->bytes && charge <= GlobalBytes - globalUsage.bytes;
    if (ok)
    {
        fs->bytes += charge;
        globalUsage.bytes += charge;
    }
    IrqSpinLockRelease(&budgetLock, irq);
    if (!ok)
        return nullptr;
    auto *p = static_cast<Allocation *>(kmalloc(charge));
    if (!p)
    {
        irq = IrqSpinLockAcquire(&budgetLock);
        fs->bytes -= charge;
        globalUsage.bytes -= charge;
        IrqSpinLockRelease(&budgetLock, irq);
        return nullptr;
    }
    p->size = charge;
    memset(p + 1, 0, bytes);
    return p + 1;
}
void Free(Tmpfs *fs, void *pointer)
{
    if (!pointer)
        return;
    auto *p = static_cast<Allocation *>(pointer) - 1;
    uint64_t charge = p->size;
    kfree(p);
    auto irq = IrqSpinLockAcquire(&budgetLock);
    if (charge > fs->bytes || charge > globalUsage.bytes)
        __builtin_trap();
    fs->bytes -= charge;
    globalUsage.bytes -= charge;
    IrqSpinLockRelease(&budgetLock, irq);
}
void Hold(Node *n)
{
    if (!n)
        return;
    auto old = __atomic_fetch_add(&n->references, 1, __ATOMIC_RELAXED);
    if (!old || old == ~uint32_t{0})
        __builtin_trap();
}
// No sleeping locks, FileObject gate or recursion on final object release.
void Drop(Node *n)
{
    while (n)
    {
        auto old = __atomic_fetch_sub(&n->references, 1, __ATOMIC_ACQ_REL);
        if (!old)
            __builtin_trap();
        if (old != 1)
            return;
        if (n->entries)
            __builtin_trap();
        auto *fs = n->fs;
        auto *parent = (n->mode & TypeMask) == Directory ? n->parent : nullptr;
        while (n->pages)
        {
            auto *p = n->pages;
            n->pages = p->next;
            Free(fs, p);
        }
        Free(fs, n->target);
        FileObjectDispose(&n->file);
        Free(fs, n);
        auto irq = IrqSpinLockAcquire(&budgetLock);
        --fs->inodes;
        --globalUsage.inodes;
        IrqSpinLockRelease(&budgetLock, irq);
        n = parent;
    }
}
int Truncate(Vnode *vn, uint64_t size);
Node *NewNode(Tmpfs *fs, uint16_t mode, uint32_t uid, uint32_t gid)
{
    auto irq = IrqSpinLockAcquire(&budgetLock);
    bool ok = fs->inodes < InstanceInodes && globalUsage.inodes < GlobalInodes;
    if (ok)
    {
        ++fs->inodes;
        ++globalUsage.inodes;
    }
    IrqSpinLockRelease(&budgetLock, irq);
    if (!ok)
        return nullptr;
    auto *n = static_cast<Node *>(Allocate(fs, sizeof(Node)));
    if (!n)
    {
        irq = IrqSpinLockAcquire(&budgetLock);
        --fs->inodes;
        --globalUsage.inodes;
        IrqSpinLockRelease(&budgetLock, irq);
        return nullptr;
    }
    n->fs = fs;
    n->references = 1;
    n->uid = uid;
    n->gid = gid;
    n->mode = mode;
    n->nlink = (mode & TypeMask) == Directory ? 2 : 1;
    if (fs->nextInode == ~uint64_t{0})
        __builtin_trap();
    n->ino = fs->nextInode++;
    n->linked = true;
    n->atime = n->mtime = n->ctime = Now();
    FileObjectInit(&n->file);
    // Strict growth cannot retire a page or modify data below old EOF.
    n->file.growMappedBacking=Truncate;
    n->file.pageBudgetContext=n;
    n->file.reservePageBytes=[](void* context,uint64_t bytes) {return Reserve(static_cast<Node*>(context)->fs,bytes);};
    n->file.releasePageBytes=[](void* context,uint64_t bytes) {Unreserve(static_cast<Node*>(context)->fs,bytes);};
    n->file.prepareBackingPage=PrepareBackingPage;
    return n;
}
// Capture process authority before the backend mutex. Group lists are immutable.
struct Actor
{
    uint32_t uid = 0, gid = 0;
    SupplementaryGroups *groups = nullptr;
    bool kernel = false;
    FileActorSnapshot authority{};
    Actor(Tmpfs *fs, bool real = false)
    {
        auto *p = ProcessCurrent();
        if (!p)
        {
            kernel = true;
            return;
        }
        authority = ProcessFileActorAcquire(p, fs->owner, real);
        uid = authority.uid;
        gid = authority.gid;
        groups = authority.groups;
    }
    ~Actor()
    {
        ProcessGroupsRelease(groups);
    }
    bool MappedUid(Node *n) const
    {
        return authority.initialNamespace || n->uid == authority.mappedUid;
    }
    bool MappedIds(Node *n) const
    {
        return MappedUid(n) && (authority.initialNamespace || n->gid == authority.mappedGid);
    }
    bool Cap(Node *n, unsigned capability, bool both = true) const
    {
        return kernel ||
               ((authority.capabilities & (uint64_t{1} << capability)) && (both ? MappedIds(n) : MappedUid(n)));
    }
    bool Group(uint32_t id) const
    {
        if (gid == id)
            return true;
        for (uint32_t i = 0; groups && i < groups->count; ++i)
            if (groups->ids[i] == id)
                return true;
        return false;
    }
    int Access(Node *n, uint8_t required) const
    {
        if (kernel)
            return 0;
        uint8_t bits = uid == n->uid ? (n->mode >> 6) & 7 : Group(n->gid) ? (n->mode >> 3) & 7 : n->mode & 7;
        if ((bits & required) == required)
            return 0;
        // A conservative namespace capability override: unrelated unmapped
        // ownership never gains powers just because a visible UID is zero.
        if (Cap(n, CapDacOverride) && (!(required & 1) || (n->mode & TypeMask) == Directory || (n->mode & 0111)))
            return 0;
        return -13;
    }
    bool Own(Node *n) const
    {
        return kernel || uid == n->uid || Cap(n, CapFowner, false);
    }
    int Sticky(Node *dir, Node *child) const
    {
        return !(dir->mode & 01000) || Own(dir) || Own(child) ? 0 : -1;
    }
};
int Leaf(const char *name)
{
    if (!name || !name[0] || !strcmp(name, ".") || !strcmp(name, ".."))
        return -22;
    for (unsigned i = 0; name[i]; ++i)
    {
        if (i >= 255)
            return -36;
        if (name[i] == '/')
            return -22;
    }
    return 0;
}
Entry **Find(Node *parent, const char *name)
{
    auto **p = &parent->entries;
    while (*p && strcmp((*p)->name, name))
        p = &(*p)->next;
    return p;
}
Node *Object(VfsObject *o)
{
    return static_cast<Node *>(o->priv);
}
Node *VnodeNode(Vnode *vn)
{
    return Object(vn->backendObject);
}
void Fill(Node *n, VnodeStat *out)
{
    *out = {};
    out->ino = n->ino;
    out->dev = n->fs->device;
    out->uid = n->uid;
    out->gid = n->gid;
    out->mode = n->mode;
    out->nlink = n->nlink;
    out->size = n->size;
    out->isDir = (n->mode & TypeMask) == Directory;
    out->isSymlink = (n->mode & TypeMask) == Link;
    out->atime = n->atime;
    out->mtime = n->mtime;
    out->ctime = n->ctime;
}
const VfsObjectOps *ObjectOperations();
int MakeObject(Node *n, VfsObject **out)
{
    *out = nullptr;
    auto *o = static_cast<VfsObject *>(Allocate(n->fs, sizeof(VfsObject)));
    if (!o)
        return -12;
    Hold(n);
    *o = {ObjectOperations(), n, n->ino, 1};
    *out = o;
    return 0;
}
void DestroyObject(VfsObject *o)
{
    auto *n = Object(o);
    Free(n->fs, o);
    Drop(n);
}
int StatObject(VfsObject *o, VnodeStat *out)
{
    if (!out)
        return -22;
    auto *n = Object(o);
    Guard g(n->fs);
    Fill(n, out);
    return 0;
}
int AccessObject(VfsObject *o, uint8_t required, bool real)
{
    auto *n = Object(o);
    Actor actor(n->fs, real);
    Guard g(n->fs);
    return required & ~7u ? -22 : actor.Access(n, required);
}
int Child(VfsObject *o, const char *name, VfsObject **out, bool real)
{
    *out = nullptr;
    if (int e = Leaf(name))
        return e;
    auto *n = Object(o);
    Actor actor(n->fs, real);
    Guard g(n->fs);
    if ((n->mode & TypeMask) != Directory)
        return -20;
    if (int e = actor.Access(n, 1))
        return e;
    auto *entry = *Find(n, name);
    return entry ? MakeObject(entry->node, out) : -2;
}
int Parent(VfsObject *o, VfsObject **out, bool real)
{
    *out = nullptr;
    auto *n = Object(o);
    Actor actor(n->fs, real);
    Guard g(n->fs);
    if ((n->mode & TypeMask) != Directory)
        return -20;
    if (int e = actor.Access(n, 1))
        return e;
    return MakeObject(n->parent ? n->parent : n, out);
}
int Readlink(VfsObject *o, char *out, uint32_t capacity)
{
    auto *n = Object(o);
    Guard g(n->fs);
    if ((n->mode & TypeMask) != Link)
        return -22;
    uint32_t count = n->size < capacity ? n->size : capacity;
    if (count)
        memcpy(out, n->target, count);
    return count;
}
int ObjectPath(VfsObject *o, char *out, uint32_t capacity)
{
    auto *n = Object(o);
    Guard g(n->fs);
    if (!n->linked)
        return -2;
    char path[512];
    uint32_t cursor = sizeof(path) - 1;
    path[cursor] = 0;
    for (unsigned depth = 0; n->parent; ++depth)
    {
        if (depth >= 256)
            return -40;
        if (!n->linked)
            return -2;
        uint32_t length = strlen(n->name);
        if (length + 1 > cursor)
            return -36;
        cursor -= length;
        memcpy(path + cursor, n->name, length);
        path[--cursor] = '/';
        n = n->parent;
    }
    if (cursor == sizeof(path) - 1)
        path[--cursor] = '/';
    uint32_t size = sizeof(path) - cursor;
    if (size > capacity)
        return -36;
    memcpy(out, path + cursor, size);
    return 0;
}
int DirectoryPath(VfsObject *o, char *out, uint32_t capacity)
{
    if ((Object(o)->mode & TypeMask) != Directory)return -20;
    return ObjectPath(o,out,capacity);
}

Page *FindPage(Node *n, uint64_t index)
{
    for (auto *p = n->pages; p; p = p->next)
        if (p->index == index)
            return p;
    return nullptr;
}
int PrepareBackingPage(void* context,uint64_t index)
{
    auto* n=static_cast<Node*>(context);
    Guard held(n->fs);
    if(FindPage(n,index))return 0;
    // Materialize sparse storage before exposing a canonical page. Writeback
    // then needs no persistent allocation, even at quota exhaustion. This can
    // leave a charged zero page if a later canonical allocation fails; it does
    // not extend EOF and ordinary inode teardown reclaims it.
    auto* page=static_cast<Page*>(Allocate(n->fs,sizeof(Page)));
    if(!page)return -28;
    page->index=index;page->next=n->pages;n->pages=page;
    return 0;
}
int Read(Vnode *vn, void *buffer, uint64_t length, uint64_t *offset)
{
    auto *n = VnodeNode(vn);
    Guard g(n->fs);
    if ((n->mode & TypeMask) != Regular)
        return -21;
    if (*offset >= n->size)
        return 0;
    if (length > 0x7fffffff)
        length = 0x7fffffff;
    if (length > n->size - *offset)
        length = n->size - *offset;
    uint64_t done = 0;
    while (done < length)
    {
        uint64_t at = *offset + done, part = 4096 - (at & 4095);
        if (part > length - done)
            part = length - done;
        auto *p = FindPage(n, at / 4096);
        if (p)
            memcpy(static_cast<uint8_t *>(buffer) + done, p->bytes + (at & 4095), part);
        else
            memset(static_cast<uint8_t *>(buffer) + done, 0, part);
        done += part;
    }
    *offset += done;
    return done;
}
int Write(Vnode *vn, const void *buffer, uint64_t length, uint64_t *offset)
{
    auto *n = VnodeNode(vn);
    Guard g(n->fs);
    if (*offset > 0x7fffffffffffffffULL || length > 0x7fffffffffffffffULL - *offset)
        return -27;
    if (length > 0x7fffffff)
        length = 0x7fffffff;
    uint64_t done = 0;
    while (done < length)
    {
        uint64_t at = *offset + done, part = 4096 - (at & 4095);
        if (part > length - done)
            part = length - done;
        auto *p = FindPage(n, at / 4096);
        if (!p)
        {
            p = static_cast<Page *>(Allocate(n->fs, sizeof(Page)));
            if (!p)
                break;
            p->index = at / 4096;
            p->next = n->pages;
            n->pages = p;
        }
        memcpy(p->bytes + (at & 4095), static_cast<const uint8_t *>(buffer) + done, part);
        done += part;
    }
    if (!done && length)
        return -28;
    *offset += done;
    if (*offset > n->size)
        n->size = *offset;
    if (done)
    {
        n->mode &= ~06000;
        n->mtime = n->ctime = Now();
    }
    return done;
}
int Truncate(Vnode *vn, uint64_t size)
{
    auto *n = VnodeNode(vn);
    Guard g(n->fs);
    if (size > 0x7fffffffffffffffULL)
        return -27;
    auto **link = &n->pages;
    while (*link)
    {
        auto *p = *link;
        if (p->index >= ((size + 4095) / 4096))
        {
            *link = p->next;
            Free(n->fs, p);
        }
        else
        {
            if (size % 4096 && p->index == size / 4096)
                memset(p->bytes + size % 4096, 0, 4096 - size % 4096);
            link = &p->next;
        }
    }
    n->size = size;
    n->mode &= ~06000;
    n->mtime = n->ctime = Now();
    return 0;
}
int StatVnode(Vnode *vn, VnodeStat *out)
{
    return StatObject(vn->backendObject, out);
}
int AccessVnode(Vnode *vn, uint8_t bits, bool real)
{
    return AccessObject(vn->backendObject, bits, real);
}
void Close(Vnode *vn)
{
    Unreserve(VnodeNode(vn)->fs, sizeof(Vnode));
}
int Sync(Vnode *)
{
    return 0;
}
FileObject *File(Vnode *vn)
{
    return &VnodeNode(vn)->file;
}
int DirPath(Vnode *vn, char *out, uint32_t cap)
{
    return DirectoryPath(vn->backendObject, out, cap);
}
int Readdir(Vnode *vn, DirEntry *out, uint32_t *cookie)
{
    auto *n = VnodeNode(vn);
    Guard g(n->fs);
    if ((n->mode & TypeMask) != Directory)
        return -20;
    *out = {};
    if (*cookie < 2)
    {
        strcpy(out->name, *cookie ? ".." : ".");
        out->isDir = true;
        ++*cookie;
        return 1;
    }
    auto *e = n->entries;
    for (uint32_t i = 2; e && i < *cookie; ++i)
        e = e->next;
    if (!e)
        return 0;
    strcpy(out->name, e->name);
    out->isDir = (e->node->mode & TypeMask) == Directory;
    out->size = e->node->size;
    ++*cookie;
    return 1;
}
int Space(Vnode *vn, VfsSpace *out)
{
    auto *fs = VnodeNode(vn)->fs;
    auto irq = IrqSpinLockAcquire(&budgetLock);
    *out = {};
    out->type = 0x01021994;
    out->blockSize = out->fragmentSize = 4096;
    out->blocks = fs->byteLimit / 4096;
    const uint64_t instanceFree=fs->byteLimit-fs->bytes;
    const uint64_t aggregateFree=GlobalBytes-globalUsage.bytes;
    out->freeBlocks = out->availableBlocks =
        (instanceFree<aggregateFree?instanceFree:aggregateFree)/4096;
    out->files = InstanceInodes;
    out->freeFiles = InstanceInodes - fs->inodes;
    out->identity = fs->device;
    out->maxName = 255;
    out->flags = fs->flags;
    IrqSpinLockRelease(&budgetLock, irq);
    return 0;
}
int Chmod(VfsObject *o, uint16_t mode)
{
    auto *n = Object(o);
    Actor actor(n->fs);
    Guard g(n->fs);
    if (!actor.Own(n))
        return -1;
    if (!actor.Cap(n, CapFsetid) && !actor.Group(n->gid))
        mode &= ~02000;
    n->mode = (n->mode & TypeMask) | (mode & 07777);
    n->ctime = Now();
    return 0;
}
int Chown(VfsObject *o, uint32_t uid, uint32_t gid)
{
    auto *n = Object(o);
    Actor actor(n->fs);
    Guard g(n->fs);
    if (!actor.Cap(n, CapChown))
        return -1;
    if (uid != ~uint32_t{0})
        n->uid = uid;
    if (gid != ~uint32_t{0})
        n->gid = gid;
    n->mode &= ~06000;
    n->ctime = Now();
    return 0;
}
int Times(VfsObject *o, const VnodeTime *times)
{
    if (!times)
        return -22;
    auto *n = Object(o);
    Actor actor(n->fs);
    Guard g(n->fs);
    bool onlyNow = true;
    for (unsigned i = 0; i < 2; ++i)
    {
        if (times[i].nsec != VFS_UTIME_NOW && times[i].nsec != VFS_UTIME_OMIT)
        {
            if (times[i].nsec < 0 || times[i].nsec >= 1000000000)
                return -22;
            onlyNow = false;
        }
    }
    if (times[0].nsec == VFS_UTIME_OMIT && times[1].nsec == VFS_UTIME_OMIT)
        return 0;
    if (!actor.Own(n) && (!onlyNow || actor.Access(n, 2)))
        return -1;
    auto now = Now();
    for (unsigned i = 0; i < 2; ++i)
    {
        if (times[i].nsec == VFS_UTIME_OMIT)
            continue;
        auto t = times[i].nsec == VFS_UTIME_NOW ? now : times[i];
        if (i)
            n->mtime = t;
        else
            n->atime = t;
    }
    n->ctime = now;
    return 0;
}
int Vchmod(Vnode *n, uint16_t m)
{
    return Chmod(n->backendObject, m);
}
int Vchown(Vnode *n, uint32_t u, uint32_t g)
{
    return Chown(n->backendObject, u, g);
}
int Vtimes(Vnode *n, const VnodeTime *t)
{
    return Times(n->backendObject, t);
}
const VnodeOps regularOps = {.open = nullptr,
                             .read = Read,
                             .write = Write,
                             .readdir = nullptr,
                             .close = Close,
                             .stat = StatVnode,
                             .fsync = Sync,
                             .set_times = Vtimes,
                             .chmod = Vchmod,
                             .truncate = Truncate,
                             .file_object = File,
                             .chown = Vchown,
                             .space = Space,
                             .access = AccessVnode,
                             .deny_mapping = false};
const VnodeOps metadataOps = {.open = nullptr,
                              .read = nullptr,
                              .write = nullptr,
                              .readdir = Readdir,
                              .close = Close,
                              .stat = StatVnode,
                              .fsync = Sync,
                              .set_times = Vtimes,
                              .chmod = Vchmod,
                              .chown = Vchown,
                              .space = Space,
                              .access = AccessVnode,
                              .directory_path = DirPath,
                              .deny_mapping = true};
Vnode *MakeVnode(Node *n, int *error)
{
    if (!Reserve(n->fs, sizeof(Vnode)))
    {
        *error = -28;
        return nullptr;
    }
    auto *vn = static_cast<Vnode *>(kmalloc(sizeof(Vnode)));
    if (!vn)
    {
        Unreserve(n->fs, sizeof(Vnode));
        *error = -12;
        return nullptr;
    }
    VfsObject *o = nullptr;
    *error = MakeObject(n, &o);
    if (*error)
    {
        kfree(vn);
        Unreserve(n->fs, sizeof(Vnode));
        return nullptr;
    }
    *vn = {};
    vn->ops = (n->mode & TypeMask) == Regular ? &regularOps : &metadataOps;
    vn->type = (n->mode & TypeMask) == Directory ? VnodeType::Dir :
        (n->mode & TypeMask) == Fifo ? VnodeType::Device : VnodeType::File;
    vn->refCount = 1;
    vn->backendObject = o;
    return vn;
}
Vnode *OpenObject(VfsObject *o, int flags, int *error)
{
    *error = 0;
    auto *n = Object(o);
    Actor actor(n->fs);
    Guard g(n->fs);
    constexpr int allowed = VFS_O_WRITE | VFS_O_READWRITE | VFS_O_APPEND | VFS_O_METADATA | VFS_O_EXEC;
    if (flags & ~allowed)
    {
        *error = -22;
        return nullptr;
    }
    uint16_t type = n->mode & TypeMask;
    if (!(flags & VFS_O_METADATA))
    {
        if (type == Link)
            *error = -40;
        else if (type == Directory && (flags & VFS_O_WRITE))
            *error = -21;
        else if ((flags & VFS_O_EXEC) && type != Regular)
            *error = -13;
        else
            *error = actor.Access(n, (flags & VFS_O_EXEC)    ? 1
                                     : (flags & VFS_O_WRITE) ? uint8_t(2 | ((flags & VFS_O_READWRITE) ? 4 : 0))
                                                             : 4);
    }
    if (*error)
        return nullptr;
    return MakeVnode(n, error);
}
int ParentCheck(Node *n, const char *name, const Actor &actor)
{
    if (int e = Leaf(name))
        return e;
    if ((n->mode & TypeMask) != Directory)
        return -20;
    if (!n->linked)
        return -2;
    return actor.Access(n, 3);
}
int Depth(Node *p)
{
    unsigned depth = 0;
    for (; p->parent; p = p->parent)
        if (++depth >= 255)
            return -40;
    return 0;
}
Entry *PrepareEntry(Node *parent, const char *name, Node *child)
{
    auto *e = static_cast<Entry *>(Allocate(parent->fs, sizeof(Entry)));
    if (!e)
        return nullptr;
    strcpy(e->name, name);
    e->node = child;
    return e;
}
void Publish(Node *parent, Entry *entry)
{
    auto *n = entry->node;
    entry->next = parent->entries;
    parent->entries = entry;
    n->parent = parent;
    strcpy(n->name, entry->name);
    if ((n->mode & TypeMask) == Directory)
    {
        Hold(parent);
        ++parent->nlink;
    }
    parent->mtime = parent->ctime = Now();
}
Vnode *Create(VfsObject *o, const char *name, int flags, uint16_t mode, int *error)
{
    *error = 0;
    auto *parent = Object(o);
    Actor actor(parent->fs);
    Guard g(parent->fs);
    if ((*error = ParentCheck(parent, name, actor)))
        return nullptr;
    if (*Find(parent, name))
    {
        *error = -17;
        return nullptr;
    }
    uint32_t gid = (parent->mode & 02000) ? parent->gid : actor.gid;
    auto *n = NewNode(parent->fs, Regular | (mode & 07777), actor.uid, gid);
    if (!n)
    {
        *error = -28;
        return nullptr;
    }
    if (!actor.Cap(n, CapFsetid) && !actor.Group(n->gid))
        n->mode &= ~02000;
    auto *e = PrepareEntry(parent, name, n);
    if (!e)
    {
        Drop(n);
        *error = -12;
        return nullptr;
    }
    auto *vn = MakeVnode(n, error);
    if (!vn)
    {
        Free(parent->fs, e);
        Drop(n);
        return nullptr;
    }
    Publish(parent, e);
    (void) flags;
    return vn;
}
Vnode *CreateFifo(VfsObject *o, const char *name, uint16_t mode, int *error)
{
    *error = 0;
    auto *parent = Object(o);
    Actor actor(parent->fs);
    Guard g(parent->fs);
    if ((*error = ParentCheck(parent, name, actor)))
        return nullptr;
    if (*Find(parent, name))
    {
        *error = -17;
        return nullptr;
    }
    uint32_t gid = (parent->mode & 02000) ? parent->gid : actor.gid;
    auto *n = NewNode(parent->fs, Fifo | (mode & 07777), actor.uid, gid);
    if (!n)
    {
        *error = -28;
        return nullptr;
    }
    if (!actor.Cap(n, CapFsetid) && !actor.Group(n->gid))
        n->mode &= ~02000;
    auto *e = PrepareEntry(parent, name, n);
    if (!e)
    {
        Drop(n);
        *error = -12;
        return nullptr;
    }
    auto *vn = MakeVnode(n, error);
    if (!vn)
    {
        Free(parent->fs, e);
        Drop(n);
        return nullptr;
    }
    Publish(parent, e);
    return vn;
}
int Mkdir(VfsObject *o, const char *name, uint16_t mode)
{
    auto *p = Object(o);
    Actor actor(p->fs);
    Guard g(p->fs);
    if (int e = ParentCheck(p, name, actor))
        return e;
    if (*Find(p, name))
        return -17;
    if (int e = Depth(p))
        return e;
    auto *n = NewNode(p->fs, Directory | (mode & 01777) | ((p->mode & 02000) ? 02000 : 0), actor.uid,
                      (p->mode & 02000) ? p->gid : actor.gid);
    if (!n)
        return -28;
    auto *e = PrepareEntry(p, name, n);
    if (!e)
    {
        Drop(n);
        return -12;
    }
    Publish(p, e);
    return 0;
}
int Symlink(VfsObject *o, const char *target, const char *name)
{
    auto *p = Object(o);
    Actor actor(p->fs);
    Guard g(p->fs);
    if (int e = ParentCheck(p, name, actor))
        return e;
    if (*Find(p, name))
        return -17;
    uint64_t length = strlen(target);
    if (!length)
        return -2;
    if (length >= 4096)
        return -36;
    auto *n = NewNode(p->fs, Link | 0777, actor.uid, (p->mode & 02000) ? p->gid : actor.gid);
    if (!n)
        return -28;
    n->target = static_cast<char *>(Allocate(p->fs, length + 1));
    if (!n->target)
    {
        Drop(n);
        return -12;
    }
    memcpy(n->target, target, length + 1);
    n->size = length;
    auto *e = PrepareEntry(p, name, n);
    if (!e)
    {
        Drop(n);
        return -12;
    }
    Publish(p, e);
    return 0;
}
// Entry owns one link reference. Removed directories retain their parent hold
// for already-open '..' traversal; iterative Drop releases that chain finally.
void RemoveEntry(Node *parent, Entry **slot)
{
    auto *e = *slot;
    *slot = e->next;
    auto *n = e->node;
    n->linked = false;
    n->nlink = 0;
    n->ctime = Now();
    if ((n->mode & TypeMask) == Directory)
        --parent->nlink;
    else
        n->parent = nullptr;
    Free(parent->fs, e);
    Drop(n);
    parent->mtime = parent->ctime = Now();
}
int Remove(VfsObject *o, const char *name, bool directory)
{
    auto *p = Object(o);
    Actor actor(p->fs);
    Guard g(p->fs);
    if (int e = ParentCheck(p, name, actor))
        return e;
    auto **slot = Find(p, name);
    if (!*slot)
        return -2;
    auto *n = (*slot)->node;
    bool dir = (n->mode & TypeMask) == Directory;
    if (dir != directory)
        return directory ? -20 : -21;
    if (dir && n->entries)
        return -39;
    if (int e = actor.Sticky(p, n))
        return e;
    RemoveEntry(p, slot);
    return 0;
}
int Rename(VfsObject *old, const char *oldName, VfsObject *next, const char *newName)
{
    auto *p = Object(old);
    auto *q = Object(next);
    if (p->fs != q->fs)
        return -18;
    Actor actor(p->fs);
    Guard g(p->fs);
    if (int e = ParentCheck(p, oldName, actor))
        return e;
    if (int e = ParentCheck(q, newName, actor))
        return e;
    auto **source = Find(p, oldName);
    if (!*source)
        return -2;
    auto *entry = *source;
    auto *n = entry->node;
    auto **dest = Find(q, newName);
    if (*dest && (*dest)->node == n)
        return 0;
    if (int e = actor.Sticky(p, n))
        return e;
    bool dir = (n->mode & TypeMask) == Directory;
    if (dir)
    {
        for (auto *ancestor = q; ancestor; ancestor = ancestor->parent)
            if (ancestor == n)
                return -22;
        // Moving a subtree must preserve the depth bound for every descendant.
        unsigned parentDepth = 0;
        for (auto *a = q; a->parent; a = a->parent)
            ++parentDepth;
        Node *walk = n;
        unsigned relative = 0;
        for (;;)
        {
            if (parentDepth + 1 + relative >= 256)
                return -40;
            Entry *child = walk->entries;
            while (child && (child->node->mode & TypeMask) != Directory)
                child = child->next;
            if (child)
            {
                walk = child->node;
                ++relative;
                continue;
            }
            for (;;)
            {
                if (walk == n)
                    goto depth_checked;
                auto *parent = walk->parent;
                auto *sibling = parent->entries;
                while (sibling && sibling->node != walk)
                    sibling = sibling->next;
                if (!sibling)
                    __builtin_trap();
                sibling = sibling->next;
                while (sibling && (sibling->node->mode & TypeMask) != Directory)
                    sibling = sibling->next;
                if (sibling)
                {
                    walk = sibling->node;
                    break;
                }
                walk = parent;
                --relative;
            }
        }
    }
depth_checked:
    if (*dest)
    {
        auto *victim = (*dest)->node;
        bool victimDir = (victim->mode & TypeMask) == Directory;
        if (dir != victimDir)
            return dir ? -20 : -21;
        if (victimDir && victim->entries)
            return -39;
        if (int e = actor.Sticky(q, victim))
            return e;
        RemoveEntry(q, dest);
    }
    // Destination removal can have invalidated a pointer into the same list.
    source = Find(p, oldName);
    *source = entry->next;
    strcpy(entry->name, newName);
    strcpy(n->name, newName);
    entry->next = q->entries;
    q->entries = entry;
    if (p != q)
    {
        if (dir)
        {
            Hold(q);
            auto *oldParent = n->parent;
            n->parent = q;
            --p->nlink;
            ++q->nlink;
            Drop(oldParent);
        }
        else
            n->parent = q;
    }
    n->ctime = p->mtime = p->ctime = q->mtime = q->ctime = Now();
    return 0;
}
const VfsObjectOps objectOps = {DestroyObject, StatObject, AccessObject, Child,  Parent,       Readlink,
                                OpenObject,    Create,     Mkdir,        Remove, Rename,       Symlink,
                                nullptr,       Times,      Chmod,        Chown,  DirectoryPath, nullptr, ObjectPath, CreateFifo};
const VfsObjectOps *ObjectOperations()
{
    return &objectOps;
}
bool RejectLegacyMount(uint8_t, void **out)
{
    *out = nullptr;
    return false;
}
int Unmount(void *fs)
{
    return TmpfsDestroy(static_cast<Tmpfs *>(fs));
}
int Root(void *fs, uint8_t, VfsObject **out)
{
    auto *f = static_cast<Tmpfs *>(fs);
    Guard g(f);
    return MakeObject(f->root, out);
}
int FromVnode(Vnode *vn, VfsObject **out)
{
    if (!vn || (vn->ops != &regularOps && vn->ops != &metadataOps))
        return -9;
    *out = vn->backendObject;
    VfsObjectRetain(*out);
    return 0;
}
int SyncFs(void *)
{
    return 0;
}
// Legacy pathname callbacks deliberately omitted: retained instance operations
// are authoritative, and unsupported global brokers must fail explicitly.
const VfsFsOps fsOps = {.mount = RejectLegacyMount,
                        .unmount = Unmount,
                        .open = nullptr,
                        .stat_path = nullptr,
                        .lstat_path = nullptr,
                        .unlink = nullptr,
                        .mkdir = nullptr,
                        .rename = nullptr,
                        .symlink = nullptr,
                        .readlink = nullptr,
                        .sync = SyncFs,
                        .root_object = Root,
                        .vnode_object = FromVnode};
} // namespace
const VfsFsOps *TmpfsOperations()
{
    return &fsOps;
}
int TmpfsConfigureByteBudget(uint64_t bytes)
{
    if(bytes<TmpfsDefaultBytes || bytes>0x7ffffffffffff000ULL || (bytes&4095))return -22;
    const auto irq=IrqSpinLockAcquire(&budgetLock);
    int result=globalUsage.mounts || globalUsage.bytes || globalUsage.inodes ? -16 : 0;
    if(!result)__atomic_store_n(&GlobalBytes,bytes,__ATOMIC_RELEASE);
    IrqSpinLockRelease(&budgetLock,irq);
    return result;
}
uint64_t TmpfsMaximumBytes()
{
    return __atomic_load_n(&GlobalBytes,__ATOMIC_ACQUIRE);
}
TmpfsUsage TmpfsGlobalUsage()
{
    auto irq = IrqSpinLockAcquire(&budgetLock);
    auto result = globalUsage;
    IrqSpinLockRelease(&budgetLock, irq);
    return result;
}
Tmpfs *TmpfsCreate(uint32_t uid, uint32_t gid, UserNamespace *owner, uint64_t flags, int *error,
                   uint16_t rootMode, uint64_t byteLimit)
{
    *error = 0;
    if ((flags & ~uint64_t{6}) || (rootMode & ~uint16_t{07777}) ||
        byteLimit < 4096 || byteLimit > TmpfsMaximumBytes() || (byteLimit & 4095))
    {
        *error = -22;
        return nullptr;
    }
    auto irq = IrqSpinLockAcquire(&budgetLock);
    if(byteLimit>GlobalBytes) {
        IrqSpinLockRelease(&budgetLock,irq);*error=-22;return nullptr;
    }
    bool ok = globalUsage.mounts < GlobalMounts && sizeof(Tmpfs) <= GlobalBytes - globalUsage.bytes &&
              nextDevice < (uint64_t{1} << 32);
    if (ok)
    {
        ++globalUsage.mounts;
        globalUsage.bytes += sizeof(Tmpfs);
    }
    uint64_t device = 0;
    if (ok)
    {
        device = nextDevice++;
    }
    IrqSpinLockRelease(&budgetLock, irq);
    if (!ok)
    {
        *error = -28;
        return nullptr;
    }
    auto *fs = static_cast<Tmpfs *>(kmalloc(sizeof(Tmpfs)));
    if (!fs)
    {
        irq = IrqSpinLockAcquire(&budgetLock);
        --globalUsage.mounts;
        globalUsage.bytes -= sizeof(Tmpfs);
        IrqSpinLockRelease(&budgetLock, irq);
        *error = -12;
        return nullptr;
    }
    *fs = {};
    fs->byteLimit = byteLimit;
    KMutexInit(&fs->mutex);
    fs->bytes = sizeof(Tmpfs);
    fs->flags = flags | 6;
    fs->device = (uint64_t{0x7e} << 8) | ((device & 0xff) | ((device & ~uint64_t{0xff}) << 12));
    fs->owner = owner;
    UserNamespaceRetain(owner);
    fs->root = NewNode(fs, Directory | rootMode, uid, gid);
    if (!fs->root)
    {
        UserNamespaceRelease(owner);
        irq = IrqSpinLockAcquire(&budgetLock);
        --globalUsage.mounts;
        globalUsage.bytes -= sizeof(Tmpfs);
        IrqSpinLockRelease(&budgetLock, irq);
        kfree(fs);
        *error = -12;
        return nullptr;
    }
    return fs;
}
int TmpfsDestroy(Tmpfs *fs)
{
    if (!fs)
        return -22;
    // All external references have drained with the owning backing. Remove link
    // edges iteratively before the root, breaking directory parent/link cycles.
    auto *current = fs->root;
    while (current->entries)
    {
        auto *e = current->entries;
        auto *child = e->node;
        if ((child->mode & TypeMask) == Directory && child->entries)
        {
            current = child;
            continue;
        }
        RemoveEntry(current, &current->entries);
        while (current != fs->root && !current->entries)
            current = current->parent;
    }
    Drop(fs->root);
    fs->root = nullptr;
    if (fs->inodes || fs->bytes != sizeof(Tmpfs))
        __builtin_trap();
    UserNamespaceRelease(fs->owner);
    auto irq = IrqSpinLockAcquire(&budgetLock);
    --globalUsage.mounts;
    globalUsage.bytes -= sizeof(Tmpfs);
    IrqSpinLockRelease(&budgetLock, irq);
    kfree(fs);
    return 0;
}
} // namespace brook
