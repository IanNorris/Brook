// syscall_fd.cpp -- descriptor table syscalls: close/close_range/dup/lseek/fcntl and descriptor teardown.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "seek_position.h"

#include "syscall.h"
#include "signal_fd.h"
#include "intel_bdw_buffer.h"
#include "memory/file_object.h"
#include "compositor_scene.h"
#include "net.h"
#include "syscall_internal.h"
#include "syscall_eventfd.h"
#include "syscall_timerfd.h"
#include "syscall_memfd.h"
#include "syscall_dsp.h"
#include "syscall_pipe.h"
#include "syscall_unix_socket.h"
#include "syscall_maintenance_block.h"
#include "syscall_drm.h"
#include "syscall_epoll.h"
#include "syscall_fd.h"
#include "syscall_netlink.h"
#include "syscall_handlers.h"
#include "nv_drm.h"

namespace brook {

void* DevKlogDeepCopy(void* handle)
{
    if (!handle) return nullptr;
    auto* src = static_cast<uint64_t*>(handle);
    auto* dst = static_cast<uint64_t*>(kmalloc(sizeof(uint64_t)));
    if (dst) *dst = *src;
    return dst;
}

void* DevTtyDeepCopy(void* handle)
{
    if (!handle) return nullptr;
    auto* src = static_cast<TtyDevicePair*>(handle);
    auto* dst = static_cast<TtyDevicePair*>(kmalloc(sizeof(TtyDevicePair)));
    if (!dst) return nullptr;
    dst->readPipe = src->readPipe;
    dst->writePipe = src->writePipe;
    // Bump pipe refcounts for the new copy
    if (dst->readPipe)
        __atomic_fetch_add(&static_cast<PipeBuffer*>(dst->readPipe)->readers, 1, __ATOMIC_RELEASE);
    if (dst->writePipe)
        __atomic_fetch_add(&static_cast<PipeBuffer*>(dst->writePipe)->writers, 1, __ATOMIC_RELEASE);
    return dst;
}

// ---------------------------------------------------------------------------
// sys_close (3)
// ---------------------------------------------------------------------------

// Tear down the handle named by an fd snapshot: per-type unref/free plus any
// waiter wakeups. Runs outside the fd table lock. Called from the immediate
// close() path (CloseProcessFd) and from FdPut when it drains the last pin on a
// slot whose close() was deferred (BRO-156). The snapshot makes this path
// independent of the slot, which by now is already cleared (or about to be).
void FinalizeClosedFd(const FdClaimResult& c)
{
    KernelOperationGuard operation(ProcessCurrent());
    // The slot's reference to its open file description (H-20). The handle
    // reference below is per slot, so the two are independent.
    OpenFileRelease(c.file);
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)
    if(c.type==FdType::BlockDevice && c.handle) {
        auto* block=static_cast<MaintenanceBlockFd*>(c.handle);
        if(__atomic_fetch_sub(&block->refs,1,__ATOMIC_ACQ_REL)==1) {
            kfree(block);MaintenanceBlockRelease();
        }
    }
#endif
    if ((c.type == FdType::Vnode || c.type == FdType::Path) && c.handle)
    {
        auto* vn = static_cast<Vnode*>(c.handle);
        uint32_t prev = __atomic_fetch_sub(&vn->refCount, 1, __ATOMIC_ACQ_REL);
        if (prev <= 1)
            VfsClose(vn);
        // else: other processes still reference this vnode
    }

    if (c.type == FdType::Pipe && c.handle)
        PipeReleaseEnd(static_cast<PipeBuffer*>(c.handle), (c.flags & 1) != 0);

    if (c.type == FdType::Socket && c.handle)
    {
        int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(c.handle)) - 1;
        brook::SockUnref(sockIdx);
    }

    if(c.type==FdType::NetlinkSocket && c.handle)NetlinkUnref(static_cast<NetlinkSocket*>(c.handle));
    if (c.type == FdType::SignalFd && c.handle) SignalFdUnref(static_cast<SignalFd*>(c.handle));
    if (c.type == FdType::SyncFile && c.handle) SyncFileUnref(static_cast<SyncFileData*>(c.handle));

    if (c.type == FdType::EventFd && c.handle)
        EventFdUnref(static_cast<EventFdData*>(c.handle));

    if (c.type == FdType::DrmPrime && c.handle)
        DrmPrimeUnref(static_cast<DrmPrimeObj*>(c.handle));
    if (c.type == FdType::CompositorScene && c.handle)
        SceneHandleUnref(static_cast<SceneHandle*>(c.handle));

    if (c.type == FdType::EpollFd && c.handle)
        EpollFdUnref(static_cast<EpollInstance*>(c.handle));

    if (c.type == FdType::TimerFd && c.handle)
        TimerFdUnref(static_cast<TimerFdData*>(c.handle));

    if (c.type == FdType::MemFd && c.handle)
    {
        auto* mfd = static_cast<MemFdData*>(c.handle);
        MemFdUnref(mfd);
    }

    if (c.type == FdType::UnixSocket && c.handle)
    {
        UNIX_LIFE(Finalize, c.handle, 0, 0, -1);
        UnixSocketHandleUnref(c.handle);
    }

    if (c.type == FdType::DevDsp && c.handle)
    {
        auto* dsp = static_cast<DspState*>(c.handle);
        uint32_t prev = __atomic_fetch_sub(&dsp->refCount, 1, __ATOMIC_ACQ_REL);
        if (prev <= 1)
        {
            AudioMixerClose(dsp->mixerStreamId);
            kfree(dsp->inputBuffer);
            kfree(dsp->buffer);
            kfree(dsp);
        }
    }

    if (c.type == FdType::DevKlog && c.handle)
    {
        // DevKlog handle is a bare uint64_t* cursor — no refcount, just free.
        // Fork deep-copies it, so each process owns its own cursor.
        kfree(c.handle);
    }

    if (c.type == FdType::DevDri && c.handle)
    {
        // Drop file-owned references. Resource retirement runs independently;
        // GEM/PRIME/VMA/capture leases independently keep the producer alive.
        auto* dctx = static_cast<DrmCtx*>(c.handle);
        if (__atomic_fetch_sub(&dctx->refCount, 1, __ATOMIC_ACQ_REL) > 1) return;
        for (uint32_t i=0;i<DRM_MAX_GEM;++i)
            if (dctx->gem[i].used) DrmGemDrop(&dctx->gem[i]);
        DrmOwnerUnref(dctx->owner);
        kfree(dctx);
    }
    if(c.type==FdType::DevIntelDri && c.handle)brook::BdwGpuFileUnref(static_cast<brook::BdwGpuFile*>(c.handle));
    if(c.type==FdType::DevNvDri && c.handle)brook::NvDrmFileUnref(static_cast<brook::NvDrmFile*>(c.handle));
    if(c.type==FdType::IntelPrime && c.handle){
        auto* buffer=static_cast<brook::BdwGpuBuffer*>(c.handle);
        brook::BdwGpuBufferTrackUserRef(buffer,brook::BdwBufferUserRef::Prime,false);
        brook::BdwGpuBufferUnref(buffer);
    }
    if(c.type==FdType::IntelSyncObj && c.handle)brook::BdwGpuSyncUnref(static_cast<brook::BdwGpuSync*>(c.handle));

    if (c.type == FdType::DevTty && c.handle)
    {
        // DevTty wraps a TtyDevicePair that references stdin/stdout pipes.
        // Opening /dev/tty incremented reader/writer counts on those pipes,
        // so closing must decrement them and free the pair struct.
        auto* pair = static_cast<TtyDevicePair*>(c.handle);
        PipeReleaseEnd(static_cast<PipeBuffer*>(pair->readPipe), false);
        PipeReleaseEnd(static_cast<PipeBuffer*>(pair->writePipe), true);
        kfree(pair);
    }
}

int64_t CloseProcessFd(Process* proc, int fd)
{
    KernelOperationGuard operation(ProcessCurrent());
    if (!proc) return -EBADF;
    // Close the slot honouring any in-flight pin. FdClose either:
    //   * ClaimedNow — slot was unpinned; it has been cleared and we own the
    //     snapshot, so finalize the handle now;
    //   * Deferred   — a sibling is mid-operation on this fd (pinned); the slot
    //     is marked closing and the last FdPut will finalize it — nothing to do
    //     here, and the caller still sees a successful close();
    //   * NotFound   — already closed (or a sibling won a concurrent close()).
    // This both prevents the double-unref the old FdGet()+FdFree() split allowed
    // and closes the use-after-free window where a blocking op dereferenced a
    // handle a sibling had freed (BRO-156).
    FdClaimResult c;
    switch (FdClose(proc, fd, &c))
    {
        case FdCloseResult::NotFound:
            return -EBADF;
        case FdCloseResult::ClaimedNow:
            UnixLifeClaim(fd, c);
            FinalizeClosedFd(c);
            return 0;
        case FdCloseResult::Deferred:
            return 0;
    }
    return 0;
}

// Force-close a slot at process teardown, IGNORING any outstanding pin. Used
// only by ProcessCloseAllFds, which runs after all sibling threads of the group
// are gone (scheduler.cpp gates it on !hasLiveThreadPeer). A thread killed while
// blocked in a pinned op (read/write/lseek) never runs its FdRef destructor, so
// pinCount stays >0 and the ordinary FdClose would defer the teardown forever —
// leaking the handle's bumped refcount. With no live peer left to call FdPut,
// it is safe to claim-and-clear the slot unconditionally and finalize now.
void FdReleaseOwnedEntry(const FdEntry& entry)
{
    FinalizeClosedFd({entry.type, entry.handle, entry.flags, entry.file});
}

int64_t CloseProcessFdForced(Process* proc, int fd)
{
    KernelOperationGuard operation(ProcessCurrent());
    if (!proc) return -EBADF;
    FdClaimResult c;
    if (FdClaim(proc, fd, &c)) {
        UnixLifeClaim(fd, c);
        FinalizeClosedFd(c);
    }
    return 0;
}

int64_t sys_close(uint64_t fd, uint64_t, uint64_t,
                   uint64_t, uint64_t, uint64_t)
{
    return CloseProcessFd(ProcessCurrent(), static_cast<int>(fd));
}

int64_t sys_close_range(uint64_t first, uint64_t last, uint64_t flags,
                        uint64_t, uint64_t, uint64_t)
{
    static constexpr uint64_t CLOSE_RANGE_UNSHARE = 1ULL << 1;
    static constexpr uint64_t CLOSE_RANGE_CLOEXEC = 1ULL << 2;
    static constexpr uint64_t CLOSE_RANGE_ALLOWED =
        CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC;

    if (flags & ~CLOSE_RANGE_ALLOWED) return -EINVAL;
    if (first > last) return -EINVAL;

    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;

    if (last >= MAX_FDS) last = MAX_FDS - 1;
    if (first >= MAX_FDS) return 0;

    for (uint64_t fd = first; fd <= last; ++fd)
    {
        if (flags & CLOSE_RANGE_CLOEXEC) {
            // Ordered against publication and close of the same slot.
            SpinLockAcquire(ProcessFdLock(proc));
            if (proc->fds[fd].type != FdType::None) proc->fds[fd].fdFlags = 1;
            SpinLockRelease(ProcessFdLock(proc));
            continue;
        }
        if (proc->fds[fd].type == FdType::None) continue;
        CloseProcessFd(proc, static_cast<int>(fd));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// sys_dup (32) / sys_dup2 (33)
// ---------------------------------------------------------------------------

// Bump refcount on the underlying object for a duplicated fd entry.
void FdBumpRefcount(const FdEntry* fde)
{
    if (!fde->handle) return;
    switch (fde->type)
    {
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)
    case FdType::BlockDevice:
        __atomic_fetch_add(&static_cast<MaintenanceBlockFd*>(fde->handle)->refs,1,__ATOMIC_RELAXED);break;
#endif
    case FdType::Pipe: {
        auto* pipe = static_cast<PipeBuffer*>(fde->handle);
        if (fde->flags & 1)
            __atomic_fetch_add(&pipe->writers, 1, __ATOMIC_RELEASE);
        else
            __atomic_fetch_add(&pipe->readers, 1, __ATOMIC_RELEASE);
        break;
    }
    case FdType::Path:
    case FdType::Vnode:
        __atomic_fetch_add(&static_cast<Vnode*>(fde->handle)->refCount, 1, __ATOMIC_RELEASE);
        break;
    case FdType::Socket: {
        int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
        brook::SockRef(sockIdx);
        break;
    }
    case FdType::NetlinkSocket:
        NetlinkRef(static_cast<NetlinkSocket*>(fde->handle));break;
    case FdType::MemFd:
        MemFdRef(static_cast<MemFdData*>(fde->handle));
        break;
    case FdType::SignalFd:
        SignalFdRef(static_cast<SignalFd*>(fde->handle));break;
    case FdType::SyncFile:
        SyncFileRef(static_cast<SyncFileData*>(fde->handle));break;
    case FdType::EventFd:
        EventFdRef(static_cast<EventFdData*>(fde->handle));
        break;
    case FdType::DevDri:
        __atomic_fetch_add(&static_cast<DrmCtx*>(fde->handle)->refCount, 1, __ATOMIC_RELEASE);
        break;
    case FdType::DevIntelDri:
        brook::BdwGpuFileRef(static_cast<brook::BdwGpuFile*>(fde->handle));break;
    case FdType::DevNvDri:
        brook::NvDrmFileRef(static_cast<brook::NvDrmFile*>(fde->handle));break;
    case FdType::IntelPrime: {
        auto* buffer=static_cast<brook::BdwGpuBuffer*>(fde->handle);
        brook::BdwGpuBufferRef(buffer);
        brook::BdwGpuBufferTrackUserRef(buffer,brook::BdwBufferUserRef::Prime,true);
        break;
    }
    case FdType::IntelSyncObj:
        brook::BdwGpuSyncRef(static_cast<brook::BdwGpuSync*>(fde->handle));break;
    case FdType::DrmPrime:
        DrmPrimeRef(static_cast<DrmPrimeObj*>(fde->handle));
        break;
    case FdType::CompositorScene:
        SceneHandleRef(static_cast<SceneHandle*>(fde->handle));
        break;
    case FdType::EpollFd:
        EpollFdRef(static_cast<EpollInstance*>(fde->handle));
        break;
    case FdType::TimerFd:
        TimerFdRef(static_cast<TimerFdData*>(fde->handle));
        break;
    case FdType::UnixSocket: {
        UnixSocketHandleRef(fde->handle);
        break;
    }
    case FdType::DevDsp:
        __atomic_fetch_add(&static_cast<DspState*>(fde->handle)->refCount, 1, __ATOMIC_ACQ_REL);
        break;
    default:
        break;
    }
}

// A successful copy owns its handle and a reference to the shared open file
// description before it can become visible in any table (dup, fork, exec
// unshare, SCM_RIGHTS). Deep-copy-only handle types must never be published
// with the source pointer. Allocation-free except for those deep copies.
bool FdRetainCopy(FdEntry* copy)
{
    if (copy->type == FdType::UnixSocket)
        UNIX_LIFE(CopyRetain, copy->handle, 0, 0, -1);
    if (copy->type == FdType::DevKlog && copy->handle) {
        void* handle = DevKlogDeepCopy(copy->handle);
        if (!handle) return false;
        copy->handle = handle;
    } else if (copy->type == FdType::DevTty && copy->handle) {
        void* handle = DevTtyDeepCopy(copy->handle);
        if (!handle) return false;
        copy->handle = handle;
    } else {
        FdBumpRefcount(copy);
    }
    OpenFileRetain(copy->file);
    return true;
}

static int64_t DuplicateFd(Process* proc, int oldfd, int target,
                           uint32_t minimum, uint8_t fdFlags)
{
    KernelOperationGuard operation(proc);
    FdRef oldref(proc, oldfd);
    if (!oldref) return -EBADF;
    if (target == oldfd) return target;
    FdEntry owned;
    SpinLockAcquire(ProcessFdLock(proc));
    owned = *oldref.get();
    bool retained = FdRetainCopy(&owned);
    SpinLockRelease(ProcessFdLock(proc));
    if (!retained) return -ENOMEM;
    owned.fdFlags = fdFlags;
    FdClaimResult retired{};
    FileLockCloseContext lockClose(proc);
    int result = FdTablePublishOwned(proc->fds, ProcessFdLock(proc), owned,
                                     minimum, target, &retired, UnixLifePublished,
                                     FileLockCloseContext::Closing,&lockClose);
    if (result < 0)
        FinalizeClosedFd({owned.type, owned.handle, owned.flags, owned.file});
    else if (retired.type != FdType::None) {
        UnixLifeClaim(result, retired);
        FinalizeClosedFd(retired);
    }
    return result;
}

int64_t sys_dup(uint64_t oldfd, uint64_t, uint64_t,
                 uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    return DuplicateFd(proc, static_cast<int>(oldfd), -1, 0, 0);
}

int64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t,
                  uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    int target = static_cast<int>(newfd);
    if (target < 0 || target >= static_cast<int>(MAX_FDS)) return -EBADF;
    return DuplicateFd(proc, static_cast<int>(oldfd), target, 0, 0);
}

int64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags,
                  uint64_t, uint64_t, uint64_t)
{
    uint32_t options = static_cast<uint32_t>(flags);
    if (options & ~0x80000u) return -EINVAL;
    if (static_cast<int>(oldfd) == static_cast<int>(newfd)) return -EINVAL;
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    int target = static_cast<int>(newfd);
    if (target < 0 || target >= static_cast<int>(MAX_FDS)) return -EBADF;
    return DuplicateFd(proc, static_cast<int>(oldfd), target, 0, options ? 1 : 0);
}

// ---------------------------------------------------------------------------
// sys_lseek (8)
// ---------------------------------------------------------------------------

static constexpr int SEEK_SET = 0;
static constexpr int SEEK_CUR = 1;
static constexpr int SEEK_END = 2;

int64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                   uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    // BRO-156: pin the slot for the whole op. SEEK_END dereferences fde->handle
    // across VfsStat, which can block on disk I/O — a bare FdGet would let a
    // sibling close() free the handle mid-stat. The offset belongs to the
    // shared open file description and is updated under its posLock.
    FdRef ref(proc, static_cast<int>(fd));
    if (!ref) return -EBADF;
    FdEntry* fde = ref.get();

    if(FdPipe(fde))return -29;
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)
    if(fde->type==FdType::BlockDevice && fde->handle) {
        auto* block=static_cast<MaintenanceBlockFd*>(fde->handle);
        KMutexLock(&block->lock);
        uint64_t base=whence==0?0:whence==1?block->position:block->bytes;
        uint64_t result=0;
        const bool ok=whence<=2 && SeekPosition(base,static_cast<int64_t>(offset),result);
        if(ok)block->position=result;
        KMutexUnlock(&block->lock);return ok?int64_t(result):-EINVAL;
    }
#endif
    // Linux noop_llseek: the position of these objects is always 0.
    if(fde->type==FdType::SignalFd || fde->type==FdType::EventFd ||
       fde->type==FdType::TimerFd || fde->type==FdType::EpollFd)return whence<=4?0:-EINVAL;
    // Streams and terminals: no_llseek (L-63).
    if(fde->type==FdType::SyncFile || fde->type==FdType::Socket ||
       fde->type==FdType::UnixSocket || fde->type==FdType::NetlinkSocket ||
       fde->type==FdType::DevTty || fde->type==FdType::DevKeyboard ||
       (fde->type==FdType::Vnode && !fde->handle))return -29; // ESPIPE
    // DMA-BUF exposes its immutable size through SEEK_END, not arbitrary seeks.
    if(fde->type==FdType::IntelPrime) {
        if(!fde->handle || offset!=0)return -EINVAL;
        if(whence==SEEK_END)return static_cast<int64_t>(brook::BdwGpuBufferBytes(static_cast<brook::BdwGpuBuffer*>(fde->handle)));
        return whence==SEEK_SET?0:-EINVAL;
    }

    if(fde->type==FdType::Path)return -EBADF;
    if(fde->type==FdType::Vnode && fde->handle && static_cast<Vnode*>(fde->handle)->type==VnodeType::Namespace)return -29;
    int64_t soff = static_cast<int64_t>(offset);
    if (whence > SEEK_END) return -EINVAL;

    FdPosition position(fde);
    uint64_t base = 0;
    switch (static_cast<int>(whence))
    {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        base = position.value();
        break;
    case SEEK_END:
    {
        if (fde->type == FdType::MemFd && fde->handle) {
            base = static_cast<MemFdData*>(fde->handle)->size;
            break;
        }
        if (fde->type != FdType::Vnode || !fde->handle) return -EINVAL;
        auto* vn = static_cast<Vnode*>(fde->handle);
        VnodeStat st{};
        if (int error = VfsStat(vn, &st); error < 0) return error;
        base = st.size;
        break;
    }
    }
    uint64_t next = 0;
    if (!SeekPosition(base, soff, next)) return -EINVAL;
    position.set(next);
    return static_cast<int64_t>(next);
}

// ---------------------------------------------------------------------------
// sys_fcntl (72)
// ---------------------------------------------------------------------------

static constexpr int F_DUPFD         = 0;
static constexpr int F_GETFD         = 1;
static constexpr int F_SETFD         = 2;
static constexpr int F_GETFL         = 3;
static constexpr int F_SETFL         = 4;
static constexpr int F_DUPFD_CLOEXEC = 1030;

static constexpr int FD_CLOEXEC = 1;

struct LinuxFlock {
    int16_t type,whence;
    uint32_t padding;
    int64_t start,length;
    int32_t pid;
    uint32_t tailPadding;
};
static_assert(sizeof(LinuxFlock)==32 && __builtin_offsetof(LinuxFlock,start)==8);
// Caller pins the descriptor and owns a kernel-operation guard throughout.
static int64_t FileRecordLock(Process* proc,FdEntry* entry,uint64_t cmd,uint64_t address) {
    if(entry->type!=FdType::Vnode || !entry->handle)return -EBADF;
    auto* vnode=static_cast<Vnode*>(entry->handle);
    if(vnode->type!=VnodeType::File || !vnode->ops->file_object)return -95;
    auto* object=vnode->ops->file_object(vnode);
    if(!object)return -95;
    if(!UserBufferReadable(address,sizeof(LinuxFlock)) ||
       (cmd==5 && !UserBufferWritable(address,sizeof(LinuxFlock))))return -EFAULT;
    LinuxFlock input;if(!CopyFromUser(&input,address,sizeof(input)))return -EFAULT;
    if(input.type<0 || input.type>2 || (cmd==5 && input.type==2))return -EINVAL;
    uint64_t size=0;
    if(input.whence==2) {
        VnodeStat stat{};int result=VfsStat(vnode,&stat);if(result)return result;size=stat.size;
    }
    RecordLockPlan plan;
    if(cmd!=5 && !plan.Prepare(input.type==2?1:2))return -37; // ENOLCK
    const auto owner=FileLockOwner(proc);
    RecordLockRange range{};RecordLockNode conflict{};
    // Lock acquisition is ordered against every logical close and dup2
    // replacement, including close of a different FD for this inode.
    SpinLockAcquire(ProcessFdLock(proc));
    int result=entry->closing?-EBADF:RecordLockNormalize(input.whence,input.start,input.length,
                                                       OpenFileOffset(entry->file),size,&range);
    if(!result && cmd!=5 && input.type!=2) {
        const auto mode=FdStatus(entry)&3;
        if((input.type==0 && mode!=0 && mode!=2) || (input.type==1 && mode!=1 && mode!=2))result=-EBADF;
    }
    bool found=false;
    if(!result) {
        if(cmd==5)found=RecordLockQuery(object->recordLocks,owner,range,input.type,&conflict);
        else {
            result=plan.Set(object->recordLocks,owner,range,input.type);
            // Explicit interim limit: do not pretend contended SETLKW waited.
            if(result==-11 && cmd==7)result=-95;
        }
    }
    SpinLockRelease(ProcessFdLock(proc));
    if(result)return result;
    if(cmd==5) {
        if(found) {
            input.type=conflict.type;input.whence=0;input.start=conflict.range.begin;
            input.length=conflict.range.end==0x7fffffffffffffffULL?0:
                         conflict.range.end-conflict.range.begin+1;
            input.pid=conflict.owner.pid;
        } else input.type=2;
        if(!CopyToUser(address,&input,sizeof(input)))return -EFAULT;
    }
    return 0;
}

int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg,
                   uint64_t, uint64_t, uint64_t)
{
    fd=static_cast<uint32_t>(fd);cmd=static_cast<uint32_t>(cmd);
    if(fd>=MAX_FDS)return -EBADF;
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    KernelOperationGuard operation(proc);
    // BRO-156: pin the slot so a sibling close() can't free it while we read or
    // mutate the FD's scalar flags (F_GETFD/F_SETFD/F_GETFL/F_SETFL) or read the
    // source slot for F_DUPFD.
    FdRef ref(proc, static_cast<int>(fd));
    if (!ref) return -EBADF;
    FdEntry* fde = ref.get();

    if(fde->type==FdType::Path && cmd!=F_DUPFD && cmd!=F_DUPFD_CLOEXEC && cmd!=F_GETFD && cmd!=F_SETFD && cmd!=F_GETFL)return -EBADF;
    switch (static_cast<int>(cmd))
    {
    case 5: // F_GETLK
    case 6: // F_SETLK
    case 7: // F_SETLKW: immediate acquisition only until cancellable waits exist.
        return FileRecordLock(proc,fde,cmd,arg);
    case 36: // F_OFD_GETLK: requires shared open-file-description ownership.
    case 37: // F_OFD_SETLK
    case 38: // F_OFD_SETLKW
        return -EINVAL;
    case 1033: // F_ADD_SEALS
    case 1034: { // F_GET_SEALS
        if(fde->type!=FdType::MemFd || !fde->handle)return -EINVAL;
        auto* mfd=static_cast<MemFdData*>(fde->handle);MfdLock(mfd);
        int result=cmd==1034?static_cast<int>(mfd->seals.bits):MemFdAddSeals(mfd->seals,static_cast<uint32_t>(arg),(FdStatus(fde)&3)!=0);
        MfdUnlock(mfd);return result;
    }
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
    {
        int minFd = static_cast<int>(arg);
        if (minFd < 0 || minFd >= static_cast<int>(MAX_FDS)) return -EINVAL;
        return DuplicateFd(proc, static_cast<int>(fd), -1,
                           static_cast<uint32_t>(minFd),
                           (cmd == F_DUPFD_CLOEXEC) ? FD_CLOEXEC : 0);
    }
    case F_GETFD:
        return fde->fdFlags;
    case F_SETFD:
        // FD_CLOEXEC is per descriptor; it never reaches dup/fork aliases.
        fde->fdFlags = static_cast<uint8_t>(arg & FD_CLOEXEC);
        return 0;
    case F_GETFL:
    {
        // Status flags belong to the open file description shared by every
        // dup/fork/SCM_RIGHTS alias (H-20).
        uint32_t status = FdStatus(fde);
        // INET status is shared by dup/fork aliases through the socket object.
        if (fde->type == FdType::Socket && fde->handle) {
            const int index = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
            status = (status & ~0x800u) | (brook::SockIsNonblock(index) ? 0x800u : 0);
        }
        if(fde->type==FdType::NetlinkSocket)status=(status&~0x800u)|(__atomic_load_n(&static_cast<NetlinkSocket*>(fde->handle)->nonblock,__ATOMIC_ACQUIRE)?0x800u:0u);
        return status;
    }
    case F_SETFL:
    {
        // Allocate packet metadata before publishing O_DIRECT on a pipe.
        if ((arg & 0x4000) && FdPipe(fde) && !FdPipe(fde)->enablePackets()) return -ENOMEM;
        // Linux SETFL_MASK (M-50): the access mode and open-only flags are
        // fixed at open. The change reaches every alias of the description.
        const uint32_t status = OpenFileUpdateStatus(fde->file, OPEN_FILE_SETFL_MASK,
                                                     static_cast<uint32_t>(arg));
        if(fde->type==FdType::NetlinkSocket)__atomic_store_n(&static_cast<NetlinkSocket*>(fde->handle)->nonblock,(status&0x800)!=0,__ATOMIC_RELEASE);
        if (fde->type == FdType::Socket && fde->handle)
            brook::SockSetNonblock(static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1,
                                  (status & 0x800) != 0);
        return 0;
    }
    default:
        return -EINVAL;
    }
}

} // namespace brook
