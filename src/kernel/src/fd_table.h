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
    uint8_t  closing;      // BRO-156: a close() raced an active pin; the slot's
                           // teardown is deferred to the last FdPut. While set,
                           // FdGetRef/Pin refuses the slot (no new users).
    uint32_t statusFlags;  // Linux O_* flags from open (for F_GETFL/F_SETFL)
    uint32_t pinCount;     // BRO-156: active fget/fput pins. A slot with pinCount>0
                           // must not be cleared or reused; close() defers until 0.
    void*    handle;       // VFS Vnode* or device-specific state
    uint64_t seekPos;      // Current file offset (for lseek)
    char     dirPath[64];  // For directory fds: path prefix for openat resolution
};

// Outcome of an FdTableClose: either the slot was claimed-and-cleared right now
// (caller must finalize *out immediately), or its teardown was deferred to the
// last in-flight FdTableUnpin, or the fd was already unused/closing.
enum class FdCloseResult : uint8_t
{
    NotFound,    // fd out of range, unused, or already closing → -EBADF
    ClaimedNow,  // slot cleared; *out filled; caller finalizes the handle now
    Deferred,    // slot still pinned; marked closing; finalize happens at last unpin
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
    e.closing     = 0;
    e.statusFlags = 0;
    e.handle      = nullptr;
    e.pinCount    = 0;
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
            fds[i].closing     = 0;
            fds[i].statusFlags = 0;
            fds[i].pinCount    = 0;
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

// ---------------------------------------------------------------------------
// Pinned fget/fput path (BRO-156).
//
// FdTableGet above returns a raw pointer after dropping the lock — a sibling
// thread can then close() the fd and free the underlying handle while the
// first thread is still dereferencing it (a UAF that widens dramatically on
// paths that *sleep* mid-operation, e.g. a 500ms blocking /dev/dsp write).
//
// The pin protocol closes that window without holding the table lock across a
// sleeping operation (which is impossible — the handle ops block):
//
//   * FdTablePin(fd)   live & not-closing → pinCount++ and return the slot.
//   * FdTableUnpin(fd) pinCount-- ; if that was the last pin AND a close was
//                      deferred, snapshot+clear the slot and return true so the
//                      caller finalizes the handle outside the lock.
//   * FdTableClose(fd) if unpinned, claim+clear immediately (ClaimedNow); if
//                      pinned, set `closing` and defer teardown to the last
//                      unpin (Deferred); otherwise NotFound.
//
// While pinned, the slot cannot be cleared or reused, so the returned pointer
// (and the handle it names) stays valid for the duration of the operation.
// ---------------------------------------------------------------------------

// Pin a live slot for use, returning a pointer that stays valid until the
// matching FdTableUnpin. Returns nullptr if the fd is out of range, unused, or
// already closing (a close is pending and no new users are admitted).
inline FdEntry* FdTablePin(FdEntry* fds, SpinLock* lock, int fd)
{
    if (fd < 0 || fd >= static_cast<int>(MAX_FDS)) return nullptr;
    SpinLockAcquire(lock);
    if (fds[fd].type == FdType::None || fds[fd].closing)
    {
        SpinLockRelease(lock);
        return nullptr;
    }
    fds[fd].pinCount++;
    SpinLockRelease(lock);
    return &fds[fd];
}

// Release a pin taken by FdTablePin. If this drops the last pin on a slot whose
// close() was deferred, snapshot the owning state into *out, clear the slot,
// and return true — the caller must then finalize (unref/free) the handle
// outside the lock, exactly as the immediate-close path does. Returns false in
// all other cases (still pinned, or no deferred close pending).
inline bool FdTableUnpin(FdEntry* fds, SpinLock* lock, int fd, FdClaimResult* out)
{
    if (fd < 0 || fd >= static_cast<int>(MAX_FDS)) return false;
    SpinLockAcquire(lock);
    if (fds[fd].pinCount > 0)
        fds[fd].pinCount--;
    bool finalize = false;
    if (fds[fd].pinCount == 0 && fds[fd].closing)
    {
        if (out)
        {
            out->type   = fds[fd].type;
            out->handle = fds[fd].handle;
            out->flags  = fds[fd].flags;
        }
        FdSlotClear(fds[fd]);
        finalize = true;
    }
    SpinLockRelease(lock);
    return finalize;
}

// Close a slot. If it is unpinned, atomically claim-and-clear it now and fill
// *out (ClaimedNow) — equivalent to FdTableClaim. If it is pinned, mark it
// `closing` (refusing further pins) and leave the owning state in place so the
// last FdTableUnpin can finalize it (Deferred). If already unused or closing,
// return NotFound. Exactly one caller across racing close()s observes a live,
// non-closing slot, so the handle is torn down exactly once.
inline FdCloseResult FdTableClose(FdEntry* fds, SpinLock* lock, int fd, FdClaimResult* out)
{
    if (fd < 0 || fd >= static_cast<int>(MAX_FDS)) return FdCloseResult::NotFound;
    SpinLockAcquire(lock);
    if (fds[fd].type == FdType::None || fds[fd].closing)
    {
        SpinLockRelease(lock);
        return FdCloseResult::NotFound;
    }
    if (fds[fd].pinCount > 0)
    {
        fds[fd].closing = 1;
        SpinLockRelease(lock);
        return FdCloseResult::Deferred;
    }
    if (out)
    {
        out->type   = fds[fd].type;
        out->handle = fds[fd].handle;
        out->flags  = fds[fd].flags;
    }
    FdSlotClear(fds[fd]);
    SpinLockRelease(lock);
    return FdCloseResult::ClaimedNow;
}

} // namespace brook
