#pragma once

// ---------------------------------------------------------------------------
// File-descriptor table primitives (BRO-156).
//
// Extracted from process.cpp so the core slot logic can be exercised by a
// real-code host test (src/tests/test_fd_table) without booting a kernel —
// the same approach used for the futex waiter pool (BRO-160).
//
// These operate directly on an FdEntry[] + its SpinLock rather than on a
// Process, so they carry no kernel dependencies beyond <stdint.h> and the
// (host-compilable) plain SpinLock. process.cpp wraps them as
// FdAlloc/FdFree/FdGet/FdClaim taking a Process*.
//
// Concurrency: every operation runs under the supplied lock. The important
// invariant for correctness on SMP is that FdTableClaim() atomically reads
// AND clears a slot, so that when two threads race to close() the same fd,
// exactly one observes a non-None slot and is therefore the sole owner
// responsible for unref'ing/freeing the underlying handle. The previous
// FdGet()+FdFree() split dropped the lock in between, allowing a double-free.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include "spinlock.h"

namespace brook {

// Maximum number of open file descriptors per process.
static constexpr uint32_t MAX_FDS = 256;

enum class FdType : uint8_t
{
    None = 0,
    Vnode,         // Regular VFS file
    DevFramebuf,   // /dev/fb0
    DevKeyboard,   // /dev/keyboard
    Pipe,          // pipe() read/write end
    DevNull,       // /dev/null — discard writes, EOF on read
    DevUrandom,    // /dev/urandom — RDRAND-backed random bytes
    SyntheticMem,  // In-memory synthetic file (e.g. /etc/passwd)
    Socket,        // Network socket (UDP/TCP)
    DevTty,        // /dev/tty — bidirectional terminal (read=stdin pipe, write=stdout pipe)
    EventFd,       // eventfd — uint64 counter for event notification
    DevDsp,        // /dev/dsp — OSS audio output
    EpollFd,       // epoll instance
    TimerFd,       // timerfd — timer-based event notification
    MemFd,         // memfd_create — anonymous in-memory file
    UnixSocket,    // AF_UNIX domain socket
    DevKlog,       // /dev/klog — kernel log ring buffer reader
};

struct FdEntry
{
    FdType   type;
    uint8_t  flags;        // O_NONBLOCK, pipe direction, etc.
    uint8_t  fdFlags;      // FD-level flags: FD_CLOEXEC (bit 0)
    uint8_t  _pad;
    uint32_t refCount;
    uint32_t statusFlags;  // Linux O_* flags from open (for F_GETFL/F_SETFL)
    void*    handle;       // VFS Vnode* or device-specific state
    uint64_t seekPos;      // Current file offset (for lseek)
    char     dirPath[64];  // For directory fds: path prefix for openat resolution
};

// Snapshot of a slot's owning state captured atomically while the slot is
// cleared. Returned by FdTableClaim so the (single) winning closer can unref
// the handle outside the lock.
struct FdClaimResult
{
    FdType   type;
    void*    handle;
    uint8_t  flags;
};

// Reset a slot to the unused state. Caller must hold the table lock.
inline void FdSlotClear(FdEntry& e)
{
    e.type        = FdType::None;
    e.flags       = 0;
    e.fdFlags     = 0;
    e.statusFlags = 0;
    e.handle      = nullptr;
    e.refCount    = 0;
    e.seekPos     = 0;
    e.dirPath[0]  = '\0';
}

// Allocate the lowest free slot for (type, handle). Returns the fd index, or
// -1 if the table is full (caller maps to -EMFILE).
inline int FdTableAlloc(FdEntry* fds, SpinLock* lock, FdType type, void* handle)
{
    SpinLockAcquire(lock);
    for (uint32_t i = 0; i < MAX_FDS; ++i)
    {
        if (fds[i].type == FdType::None)
        {
            fds[i].type        = type;
            fds[i].flags       = 0;
            fds[i].fdFlags     = 0;
            fds[i].refCount    = 1;
            fds[i].statusFlags = 0;
            fds[i].handle      = handle;
            fds[i].seekPos     = 0;
            fds[i].dirPath[0]  = '\0';
            SpinLockRelease(lock);
            return static_cast<int>(i);
        }
    }
    SpinLockRelease(lock);
    return -1;
}

// Unconditionally clear a slot. Used by paths that have already taken
// ownership of the handle (e.g. dup2 replacing a target after claiming it).
inline void FdTableFree(FdEntry* fds, SpinLock* lock, int fd)
{
    if (fd < 0 || fd >= static_cast<int>(MAX_FDS)) return;
    SpinLockAcquire(lock);
    FdSlotClear(fds[fd]);
    SpinLockRelease(lock);
}

// Return a pointer to a live slot, or nullptr if the fd is out of range or
// unused. NOTE: this drops the lock before returning — it remains subject to
// the TOCTOU described in BRO-156 for callers that dereference the handle.
// New code touching the handle should migrate to the ref-counted fget/fput
// path; FdTableGet survives for scalar-only callers.
inline FdEntry* FdTableGet(FdEntry* fds, SpinLock* lock, int fd)
{
    if (fd < 0 || fd >= static_cast<int>(MAX_FDS)) return nullptr;
    SpinLockAcquire(lock);
    if (fds[fd].type == FdType::None)
    {
        SpinLockRelease(lock);
        return nullptr;
    }
    SpinLockRelease(lock);
    return &fds[fd];
}

// Atomically read-and-clear a slot. Returns true and fills *out exactly once
// across concurrent callers racing on the same fd; all other callers see the
// slot already None and return false. This makes close() safe against a
// sibling thread closing the same fd: only the winner unref's the handle.
inline bool FdTableClaim(FdEntry* fds, SpinLock* lock, int fd, FdClaimResult* out)
{
    if (fd < 0 || fd >= static_cast<int>(MAX_FDS)) return false;
    SpinLockAcquire(lock);
    if (fds[fd].type == FdType::None)
    {
        SpinLockRelease(lock);
        return false;
    }
    if (out)
    {
        out->type   = fds[fd].type;
        out->handle = fds[fd].handle;
        out->flags  = fds[fd].flags;
    }
    FdSlotClear(fds[fd]);
    SpinLockRelease(lock);
    return true;
}

} // namespace brook
