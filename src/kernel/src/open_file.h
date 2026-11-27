#pragma once

// ---------------------------------------------------------------------------
// Shared open file description (refactor issue H-20).
//
// Linux separates the per-descriptor FD_CLOEXEC bit from the open file
// description that open() creates. dup/dup2/dup3/F_DUPFD, fork and SCM_RIGHTS
// make a new descriptor that names the SAME description, so the file offset
// and the file status flags (O_APPEND, O_NONBLOCK, ...) are shared: after
// `cmd >f 2>&1` both descriptors append at one offset, and a forked child's
// F_SETFL O_NONBLOCK is visible to its parent. Two independent open() calls
// create two descriptions.
//
// Brook keeps the immutable identity of the description (type, handle, pipe
// direction) in each FdEntry, with the per-type handle reference retained per
// slot as before. Only the mutable shared state lives here. Every live FdEntry
// owns one reference to its OpenFile; FinalizeClosedFd releases it.
//
// Offset protocol: read/write/lseek/getdents64 on regular files and
// directories (and memfd / synthetic files) hold posLock across the
// read-modify-write of `offset`, as Linux does with f_pos_lock for
// FMODE_ATOMIC_POS files. posLock is a sleeping lock because VFS I/O blocks.
// Pipes, sockets and other stream objects never take it. Snapshot readers
// (/proc/<pid>/fdinfo, F_GETLK SEEK_CUR, sendfile) use atomic loads.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include "sync/kmutex.h"

extern "C" void* kmalloc(uint64_t size);
extern "C" void kfree(void* ptr);

namespace brook {

// open(2) flags that describe the call rather than the description. Linux
// never reports them through F_GETFL.
static constexpr uint32_t OPEN_FILE_CALL_ONLY_FLAGS =
    0x40u | 0x80u | 0x100u | 0x200u | 0x80000u; // O_CREAT|O_EXCL|O_NOCTTY|O_TRUNC|O_CLOEXEC

// fcntl(F_SETFL) may change only these bits (Linux SETFL_MASK).
static constexpr uint32_t OPEN_FILE_SETFL_MASK =
    0x400u | 0x800u | 0x2000u | 0x4000u | 0x40000u; // O_APPEND|O_NONBLOCK|O_ASYNC|O_DIRECT|O_NOATIME

struct OpenFile
{
    uint32_t refs;         // one per FdEntry or in-flight SCM_RIGHTS record
    uint32_t statusFlags;  // access mode + O_* status flags; atomic access only
    uint64_t offset;       // file position; RMW under posLock
    KMutex   posLock;      // sleeping: held across blocking VFS I/O
    uint64_t identity;     // never reused (epoll keys registrations by fd number + description)
};
inline uint64_t g_openFileIdentity = 0;
inline uint64_t OpenFileIdentity(const OpenFile* file) { return file ? file->identity : 0; }

// A new description with one reference, offset 0, and the open flags minus
// the call-only bits. Returns nullptr when out of memory.
inline OpenFile* OpenFileCreate(uint32_t openFlags)
{
    auto* file = static_cast<OpenFile*>(kmalloc(sizeof(OpenFile)));
    if (!file) return nullptr;
    file->refs = 1;
    file->statusFlags = openFlags & ~OPEN_FILE_CALL_ONLY_FLAGS;
    file->offset = 0;
    file->identity = __atomic_add_fetch(&g_openFileIdentity, 1, __ATOMIC_RELAXED);
    KMutexInit(&file->posLock);
    return file;
}

inline void OpenFileRetain(OpenFile* file)
{
    if (file) __atomic_fetch_add(&file->refs, 1, __ATOMIC_RELAXED);
}

inline void OpenFileRelease(OpenFile* file)
{
    if (file && __atomic_fetch_sub(&file->refs, 1, __ATOMIC_ACQ_REL) == 1)
        kfree(file);
}

inline uint32_t OpenFileStatus(const OpenFile* file)
{
    return file ? __atomic_load_n(&file->statusFlags, __ATOMIC_ACQUIRE) : 0;
}

// Atomically replace the bits in `mask` with `bits & mask`; returns the new
// status. Concurrent F_SETFL/FIONBIO from any process sharing the
// description never lose each other's unrelated bits.
inline uint32_t OpenFileUpdateStatus(OpenFile* file, uint32_t mask, uint32_t bits)
{
    if (!file) return 0;
    uint32_t old = __atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED);
    uint32_t next;
    do {
        next = (old & ~mask) | (bits & mask);
    } while (!__atomic_compare_exchange_n(&file->statusFlags, &old, next, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
    return next;
}

inline uint64_t OpenFileOffset(const OpenFile* file)
{
    return file ? __atomic_load_n(&file->offset, __ATOMIC_RELAXED) : 0;
}

inline void OpenFileSetOffset(OpenFile* file, uint64_t offset)
{
    if (file) __atomic_store_n(&file->offset, offset, __ATOMIC_RELAXED);
}

// Scoped access to a description's offset for one positioned operation.
// `serialize` is false for objects whose reads may block indefinitely
// (devices, pseudo files) so they cannot stall lseek on the same description.
class OpenFilePosition
{
public:
    explicit OpenFilePosition(OpenFile* file, bool serialize = true)
        : file_(file), locked_(file && serialize)
    {
        if (locked_) KMutexLock(&file_->posLock);
    }
    ~OpenFilePosition()
    {
        if (locked_) KMutexUnlock(&file_->posLock);
    }
    OpenFilePosition(const OpenFilePosition&) = delete;
    OpenFilePosition& operator=(const OpenFilePosition&) = delete;

    // Live offset of the description. A descriptor without a description
    // (never published by the kernel) gets a private scratch position.
    uint64_t* get() { return file_ ? &file_->offset : &scratch_; }
    uint64_t value() const { return file_ ? file_->offset : scratch_; }
    void set(uint64_t offset) { *get() = offset; }

private:
    OpenFile* file_;
    bool locked_;
    uint64_t scratch_ = 0;
};

} // namespace brook
