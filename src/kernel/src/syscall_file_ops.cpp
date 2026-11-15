// syscall_file_ops.cpp -- per-file operations: truncate, fallocate, flock, chmod/chown, utimensat, sync/fsync.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "process_user_namespace.h"

#include "memory/virtual_memory.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_memfd.h"
#include "syscall_pipe.h"
#include "syscall_maintenance_block.h"
#include "syscall_open.h"
#include "syscall_handlers.h"

namespace brook {

int64_t sys_sync(uint64_t, uint64_t, uint64_t,
                 uint64_t, uint64_t, uint64_t)
{
    return VfsSync(nullptr);
}

int64_t sys_fsync(uint64_t fd, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef ref(proc, static_cast<int>(fd));
    FdEntry* fde = ref.get();
    if (!fde || fde->type == FdType::None || fde->type==FdType::Path) return -EBADF;

#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)
    if(fde->type==FdType::BlockDevice && fde->handle)
        return DeviceFlush(static_cast<MaintenanceBlockFd*>(fde->handle)->device);
#endif

    if (fde->type == FdType::Vnode && fde->handle)
        return VfsFsync(static_cast<Vnode*>(fde->handle));

    // Devices, sockets, pipes, memfds, and synthetic files have no persistent
    // backing store in Brook yet. Media stacks often call fdatasync() as a
    // harmless flush hint; treating it as complete avoids false fatal errors.
    return 0;
}

// ---------------------------------------------------------------------------
// sys_ftruncate (77) / sys_truncate (76) — change file size
// ---------------------------------------------------------------------------

int64_t sys_ftruncate(uint64_t fd, uint64_t length, uint64_t,
                       uint64_t, uint64_t, uint64_t)
{
    if (length > 0x7fffffffffffffffULL) return -EINVAL;
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef ref(proc, static_cast<int>(fd));
    if (!ref || !ref.get()->handle) return -EBADF;
    FdEntry* fde = ref.get();
    if(fde->type==FdType::Path)return -EBADF;
    if (fde->type == FdType::Vnode) {
        if ((fde->statusFlags & 3) == 0) return -EINVAL;
        return VfsTruncate(static_cast<Vnode*>(fde->handle), length);
    }
    if (fde->type == FdType::MemFd) {
            if ((fde->statusFlags & 3) == 0) return -EINVAL;
            auto* mfd = static_cast<MemFdData*>(fde->handle);
            MfdLock(mfd);
            if(int error=MemFdSealedResize(mfd->seals,mfd->size,length)) {MfdUnlock(mfd);return error;}
            if (!MemFdGrowLocked(mfd, length)) { MfdUnlock(mfd); return -ENOMEM; }
            // Shrinking ftruncate: free pages past the new tail. (Growth is
            // free — pages are unbacked / read-as-zero until first access.)
            if (length < mfd->size) {
                const uint64_t tail=length&4095,index=length/4096;
                if(tail && index<mfd->pageMapCount && mfd->pageMap[index])
                    memset(reinterpret_cast<void*>(PhysToVirt(PhysicalAddress(mfd->pageMap[index])).raw()+tail),0,4096-tail);
                uint64_t firstFreeIdx = (length + 4095) / 4096;
                for (uint64_t i = firstFreeIdx; i < mfd->pageMapCount; i++) {
                    if (mfd->pageMap[i]) {
                        PmmFreePage(PhysicalAddress(mfd->pageMap[i]));
                        mfd->pageMap[i] = 0;
                    }
                }
            }
            mfd->size = length;
            MfdUnlock(mfd);
            return 0;
    }
    return -EINVAL;
}

int64_t sys_truncate(uint64_t pathAddr, uint64_t length, uint64_t,
                      uint64_t, uint64_t, uint64_t)
{
    if (length > 0x7fffffffffffffffULL) return -EINVAL;
    char path[256];
    if (!CopyUserCString(pathAddr, path, sizeof(path))) return -EFAULT;
    if (!path[0]) return -ENOENT;
    AtLookup lookup;if(int error=lookup.Init(-100,path))return error;
    int result=0;
    Vnode* vn=VfsOpenAt(lookup.fs.root,lookup.start(),path,VFS_O_WRITE,0,lookup.fs.mask,&result);
    if(!vn)return result?result:-ENOENT;
    result = VfsTruncate(vn, length);
    VfsClose(vn);
    return result;
}

// ---------------------------------------------------------------------------
// sys_flock (73) — advisory file locking (stub: always succeeds)
// ---------------------------------------------------------------------------

int64_t sys_flock(uint64_t fd, uint64_t operation, uint64_t,
                   uint64_t, uint64_t, uint64_t)
{
    (void)fd; (void)operation;
    // Advisory locking — with a single-user OS, always succeed
    return 0;
}

// ---------------------------------------------------------------------------
// sys_chmod (90) / sys_fchmod (91) / sys_fchmodat (268)
// — dispatch mode changes through the file's actual mount
// ---------------------------------------------------------------------------

int64_t sys_chmod(uint64_t pathAddr, uint64_t mode, uint64_t,
                   uint64_t, uint64_t, uint64_t)
{
    char path[512];if(!CopyUserCString(pathAddr,path,sizeof(path)))return -EFAULT;
    AtLookup lookup;if(int error=lookup.Init(-100,path))return error;
    return VfsChmodAt(lookup.fs.root,lookup.start(),path,static_cast<uint16_t>(mode&07777));
}

int64_t sys_fchmod(uint64_t fd, uint64_t mode, uint64_t,
                    uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    FdRef ref(proc,static_cast<int>(fd));FdEntry* entry=ref.get();
    if (!entry || !entry->handle || entry->type==FdType::Path) return -EBADF;
    if (entry->type != FdType::Vnode) return -95; // EOPNOTSUPP
    return VfsChmod(static_cast<Vnode*>(entry->handle), static_cast<uint16_t>(mode & 07777));
}

int64_t sys_fchmodat(uint64_t dirfd, uint64_t pathAddr, uint64_t mode,
                      uint64_t, uint64_t, uint64_t)
{
    char path[512];if(!CopyUserCString(pathAddr,path,sizeof(path)))return -EFAULT;
    AtLookup lookup;if(int error=lookup.Init(static_cast<int>(dirfd),path))return error;
    return VfsChmodAt(lookup.fs.root,lookup.start(),path,static_cast<uint16_t>(mode&07777));
}

// ---------------------------------------------------------------------------
// sys_chown (92) / sys_fchown (93) / sys_lchown (94) / sys_fchownat (260)
// — resolve caller IDs, then authorize against the retained backend owner
// ---------------------------------------------------------------------------

static int64_t ChownAt(int dirfd, uint64_t pathAddr, uint32_t uid, uint32_t gid, uint64_t flags)
{
    constexpr uint64_t NOFOLLOW=0x100,EMPTY_PATH=0x1000;
    if(flags & ~(NOFOLLOW|EMPTY_PATH))return -EINVAL;
    Process* proc=ProcessCurrent();if(!proc)return -EPERM;
    if(uid!=0xffffffffu){uid=ProcessResolveId(proc,false,uid);if(uid==UserIdInvalid)return -EINVAL;}
    if(gid!=0xffffffffu){gid=ProcessResolveId(proc,true,gid);if(gid==UserIdInvalid)return -EINVAL;}
    char path[512];if(!CopyUserCString(pathAddr,path,sizeof(path)))return -EFAULT;
    if(!path[0]) {
        if(!(flags&EMPTY_PATH))return -ENOENT;
        if(dirfd==-100) {
            FsSnapshot fs{};int error=ProcessFsSnapshot(proc,&fs);
            if(!error)error=VfsChownAt(fs.root,fs.cwd,".",uid,gid);
            FsSnapshotRelease(&fs);return error;
        }
        FdRef ref(proc,dirfd);if(!ref)return -EBADF;
        if(ref->type!=FdType::Vnode || !ref->handle)return -95;
        return VfsChown(static_cast<Vnode*>(ref->handle),uid,gid);
    }
    AtLookup lookup;if(int error=lookup.Init(dirfd,path))return error;
    return VfsChownAt(lookup.fs.root,lookup.start(),path,uid,gid,!(flags&NOFOLLOW));
}

int64_t sys_chown(uint64_t pathAddr, uint64_t uid, uint64_t gid,
                   uint64_t, uint64_t, uint64_t)
{
    return ChownAt(-100, pathAddr, static_cast<uint32_t>(uid), static_cast<uint32_t>(gid), 0);
}

int64_t sys_fchown(uint64_t fd, uint64_t uid, uint64_t gid,
                    uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc)return -EBADF;
    FdRef ref(proc, static_cast<int>(fd));
    if (!ref || ref->type==FdType::Path) return -EBADF;
    if (ref->type != FdType::Vnode) return -95;
    uint32_t owner=static_cast<uint32_t>(uid),group=static_cast<uint32_t>(gid);
    if(owner!=0xffffffffu){owner=ProcessResolveId(proc,false,owner);if(owner==UserIdInvalid)return -EINVAL;}
    if(group!=0xffffffffu){group=ProcessResolveId(proc,true,group);if(group==UserIdInvalid)return -EINVAL;}
    return VfsChown(static_cast<Vnode*>(ref->handle),owner,group);
}

int64_t sys_lchown(uint64_t pathAddr, uint64_t uid, uint64_t gid,
                    uint64_t, uint64_t, uint64_t)
{
    return ChownAt(-100, pathAddr, static_cast<uint32_t>(uid), static_cast<uint32_t>(gid), 0x100);
}

int64_t sys_fchownat(uint64_t dirfd, uint64_t pathAddr, uint64_t uid,
                      uint64_t gid, uint64_t flags, uint64_t)
{
    return ChownAt(static_cast<int>(dirfd), pathAddr, static_cast<uint32_t>(uid), static_cast<uint32_t>(gid), flags);
}

// ---------------------------------------------------------------------------
// sys_utimensat (280) — change file timestamps
// ---------------------------------------------------------------------------

int64_t sys_utimensat(uint64_t dirfd, uint64_t pathAddr, uint64_t timesAddr,
                       uint64_t flags, uint64_t, uint64_t)
{
    constexpr uint64_t NOFOLLOW = 0x100;
    constexpr uint64_t EMPTY_PATH = 0x1000;
    if (flags & ~(NOFOLLOW | EMPTY_PATH)) return -EINVAL;
    VnodeTime times[2] = {{0, VFS_UTIME_NOW}, {0, VFS_UTIME_NOW}};
    if (timesAddr) {
        struct UserTime { int64_t sec, nsec; };
        UserTime userTimes[2];
        if (!CopyFromUser(userTimes, timesAddr, sizeof(userTimes))) return -EFAULT;
        for (int i = 0; i < 2; ++i) {
            UserTime value = userTimes[i];
            if (value.nsec != VFS_UTIME_NOW && value.nsec != VFS_UTIME_OMIT &&
                (value.nsec < 0 || value.nsec >= 1000000000)) return -EINVAL;
            times[i] = {value.sec, static_cast<int32_t>(value.nsec)};
        }
    }
    if (times[0].nsec == VFS_UTIME_OMIT && times[1].nsec == VFS_UTIME_OMIT) return 0;
    char path[256] = {};
    if (pathAddr && !CopyUserCString(pathAddr, path, sizeof(path))) return -EFAULT;
    if (!pathAddr || (!path[0] && (flags & EMPTY_PATH))) {
        if (flags & NOFOLLOW) return -EINVAL;
        Process* proc = ProcessCurrent();
        FdRef descriptor(proc,static_cast<int>(dirfd));
        FdEntry* entry = descriptor.get();
        if (!entry || !entry->handle || entry->type != FdType::Vnode) return -EBADF;
        return VfsSetTimes(static_cast<Vnode*>(entry->handle), times);
    }
    if (!path[0]) return -ENOENT;
    AtLookup lookup;if(int error=lookup.Init(static_cast<int>(dirfd),path))return error;
    return VfsSetTimesAt(lookup.fs.root,lookup.start(),path,times,!(flags&NOFOLLOW));
}

// ---------------------------------------------------------------------------
// sys_fallocate (285) — real memfd reservation, not sparse logical growth.
// ---------------------------------------------------------------------------

int64_t sys_fallocate(uint64_t fd, uint64_t mode, uint64_t offset,
                       uint64_t length, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    KernelOperationGuard operation(proc);
    FdRef descriptor(proc, static_cast<int>(fd)); // Linux fd/mode are low-32-bit ints.
    if (!descriptor) return -EBADF;
    FdEntry* entry = descriptor.get();
    if ((entry->statusFlags & 3u) == 0) return -EBADF;
    if (static_cast<int64_t>(offset) < 0 || static_cast<int64_t>(length) <= 0)
        return -EINVAL;
    if (offset > 0x7fffffffffffffffULL - length) return -27; // EFBIG
    uint32_t options = static_cast<uint32_t>(mode);
    if (options & ~1u) return -95; // EOPNOTSUPP: only KEEP_SIZE is supported.
    if (FdPipe(entry)) return -29; // ESPIPE
    if (entry->type == FdType::Vnode) return -95; // Backends have no reservation operation.
    if (entry->type != FdType::MemFd || !entry->handle) return -ENODEV;

    uint64_t end = offset + length;
    if (end > 16ULL * 1024 * 1024 * 1024) return -27; // Existing memfd index limit.
    auto* mfd = static_cast<MemFdData*>(entry->handle);
    MfdLock(mfd);
    // Linux forbids beyond-EOF reservation under GROW even with KEEP_SIZE.
    if(end>mfd->size && (mfd->seals.bits&4)) {MfdUnlock(mfd);return -EPERM;}
    if (!MemFdGrowLocked(mfd, end)) { MfdUnlock(mfd); return -ENOMEM; }

    // Stage only new physical pages in a private chain. Its two temporary
    // words are never published to the backing map or users. On OOM every
    // staged page is freed; existing bytes, size and seek position stay intact.
    uint64_t staged = 0;
    uint64_t first = offset / 4096;
    uint64_t last = (end - 1) / 4096;
    for (uint64_t index = first; index <= last; ++index) {
        if (mfd->pageMap[index]) continue;
        PhysicalAddress page = PmmAllocPage(MemTag::User, 0);
        if (!page) {
            while (staged) {
                auto* words = reinterpret_cast<uint64_t*>(PhysToVirt(PhysicalAddress(staged)).raw());
                uint64_t next = words[0];
                PmmFreePage(PhysicalAddress(staged));
                staged = next;
            }
            MfdUnlock(mfd);
            return -28; // ENOSPC: reservation could not be satisfied.
        }
        auto* words = reinterpret_cast<uint64_t*>(PhysToVirt(page).raw());
        words[0] = staged;
        words[1] = index;
        staged = page.raw();
    }
    while (staged) {
        auto* words = reinterpret_cast<uint64_t*>(PhysToVirt(PhysicalAddress(staged)).raw());
        uint64_t next = words[0], index = words[1];
        memset(words, 0, 4096);
        mfd->pageMap[index] = staged;
        staged = next;
    }
    if (!(options & 1u) && end > mfd->size) mfd->size = end;
    MfdUnlock(mfd);
    return 0;
}

} // namespace brook
