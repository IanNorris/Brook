#pragma once

// syscall_internal.h -- internal declarations shared from syscall_common.cpp.
// Internal to the syscall layer; split from syscall.cpp.

#include "serial.h"
#include "scheduler.h"
#include "vfs.h"
uint64_t SoftRandU64();
bool RdrandU64(uint64_t* out);

namespace brook {

extern volatile uint64_t g_profOpenCount;
extern volatile uint64_t g_profMmapCount;
extern volatile uint64_t g_profFaultCount;  // defined in idt.cpp
extern volatile uint64_t g_profCacheHit;
extern volatile uint64_t g_profCacheMiss;
extern volatile uint64_t g_profPreloadPages;
static constexpr int64_t ENOENT  = 2;
static constexpr int64_t EEXIST  = 17;
static constexpr int64_t ESRCH   = 3;
static constexpr int64_t E2BIG   = 7;
static constexpr int64_t EPERM   = 1;
static constexpr int64_t EBADF   = 9;
static constexpr int64_t ENOMEM  = 12;
static constexpr int64_t EFAULT  = 14;
static constexpr int64_t ENODEV  = 19;
static constexpr int64_t EINVAL  = 22;
static constexpr int64_t ENOEXEC = 8;
static constexpr int64_t EMFILE  = 24;
static constexpr int64_t EPIPE   = 32;
static constexpr int64_t ERANGE  = 34;
static constexpr int64_t ENOSYS  = 38;
static constexpr int64_t EAGAIN  = 11;
static constexpr int64_t EINTR   = 4;
static constexpr int64_t ENOTDIR = 20;
static constexpr int64_t EIO     = 5;
static constexpr int64_t ENOTSOCK = 88;
static constexpr int64_t ENOTCONN = 107;
static constexpr int64_t EAFNOSUPPORT = 97;
static constexpr int64_t ECONNREFUSED = 111;
static constexpr int64_t ETIMEDOUT    = 110;
static constexpr int64_t ELOOP       = 40;
bool HasPendingSignals();
bool HasReadInterruptSignal();
void WakeProcess(Process* proc);

struct KernelOperationGuard {
    Process* proc;
    explicit KernelOperationGuard(Process* p) : proc(p) { if (proc) SchedulerKernelRegionEnter(proc); }
    ~KernelOperationGuard() { if (proc) SchedulerKernelRegionLeave(proc); }
};

static inline void VnodeRef(Vnode* vn)
{
    if (!vn) return;
    // Detect use-after-free: kfree poisons memory with 0xDF bytes.
    // If the ops pointer is the poison pattern, the vnode was already freed.
    uintptr_t opsVal = reinterpret_cast<uintptr_t>(vn->ops);
    if (opsVal == 0xdfdfdfdfdfdfdfdfULL || vn->refCount == 0) {
        SerialPrintf("BUG: VnodeRef on freed/zero-ref vnode %p (ops=%p refCount=%u)\n",
                     vn, vn->ops, vn->refCount);
        return; // refuse to ref a dead vnode
    }
    VfsRetain(vn);
}

// Try to take a reference on a vnode that might be concurrently freed.
// Returns true if the ref was taken (refCount was > 0 and is now +1).
// Returns false if refCount was already 0 (vnode is being freed).

static inline void VnodeUnref(Vnode* vn)
{
    if (!vn) return;
    uint32_t prev = __atomic_fetch_sub(&vn->refCount, 1, __ATOMIC_ACQ_REL);
    if (prev <= 1)
        VfsClose(vn);
}

// RAII guard for the pinned fget/fput path (BRO-156). Constructing it pins the
// fd; destruction (on *every* return path, including the blocking ones) runs
// the matching FdPut, which performs any deferred-close teardown. Use this
// instead of a bare FdGet anywhere the handle is dereferenced across a point
// where the operation can sleep — a sibling thread could otherwise close() the
// fd and free the handle mid-use.
struct FdRef
{
    Process* proc;
    int      fd;
    FdEntry* fde;

    FdRef(Process* p, int f) : proc(p), fd(f), fde(FdGetRef(p, f)) {}
    ~FdRef() { if (fde) FdPut(proc, fd); }

    FdRef(const FdRef&)            = delete;
    FdRef& operator=(const FdRef&) = delete;

    explicit operator bool() const { return fde != nullptr; }
    FdEntry* operator->() const     { return fde; }
    FdEntry* get() const            { return fde; }
};

// Linux FMODE_ATOMIC_POS: offset updates on regular files and directories
// (and memfd / synthetic files) serialize on the description's posLock, so
// aliases created by dup/fork never interleave a read-modify-write of the
// shared offset. Devices and pseudo files may block indefinitely and do not.
inline bool FdAtomicPosition(const FdEntry* fde)
{
    if (fde->type == FdType::MemFd || fde->type == FdType::SyntheticMem) return true;
    if (fde->type != FdType::Vnode || !fde->handle) return false;
    const VnodeType type = static_cast<const Vnode*>(fde->handle)->type;
    return type == VnodeType::File || type == VnodeType::Dir;
}

// Scoped offset of the descriptor's open file description (see open_file.h).
struct FdPosition : OpenFilePosition
{
    explicit FdPosition(const FdEntry* fde) : OpenFilePosition(fde->file, FdAtomicPosition(fde)) {}
};

bool StrEq(const char* a, const char* b);

} // namespace brook
