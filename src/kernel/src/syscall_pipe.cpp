// syscall_pipe.cpp -- pipes and named FIFOs: pipe/pipe2, pipe end release and wakeups.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "syscall.h"
#include "apic.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_pipe.h"
#include "syscall_fd.h"
#include "syscall_handlers.h"

namespace brook {

#include "pipe_poll.inc"

void PipeReleaseEnd(PipeBuffer* pipe, bool writer)
{
    if (!pipe) return;
    PipeBufferHold(pipe);
    // Finish every access before surrendering this endpoint. An opposite-end
    // closer cannot destroy the object while we still hold its lock/reference.
    SpinLockAcquire(&pipe->lock);
    if (writer)
        __atomic_fetch_sub(&pipe->writers, 1, __ATOMIC_RELEASE);
    else
        __atomic_fetch_sub(&pipe->readers, 1, __ATOMIC_RELEASE);
    Process* waiter = writer ? pipe->readerWaiter : pipe->writerWaiter;
    if (writer) pipe->readerWaiter = nullptr;
    else pipe->writerWaiter = nullptr;
    if (waiter) WakeProcess(waiter);
    PipeWakeEpollRaw(pipe);
    bool destroy = __atomic_load_n(&pipe->readers, __ATOMIC_ACQUIRE) == 0 &&
                   __atomic_load_n(&pipe->writers, __ATOMIC_ACQUIRE) == 0;
    SpinLockRelease(&pipe->lock);
    PipePollNotify(pipe, 1 | 4);
    PipeBufferDrop(pipe, destroy);
}

#include "named_fifo.inc"

// ---------------------------------------------------------------------------
// sys_pipe (22)
// ---------------------------------------------------------------------------

static int64_t CreatePipe(uint64_t pipefdAddr, uint64_t flags)
{
    if (!UserBufferWritable(pipefdAddr, 2 * sizeof(int32_t))) return -EFAULT;
    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;

    // Allocate pipe buffer
    auto* pipe = PipeBufferCreate(PIPE_BUF_DEFAULT_SIZE);
    if (!pipe) return -ENOMEM;
    if ((flags & 0x4000) && !pipe->enablePackets()) {PipeBufferDestroy(pipe);return -ENOMEM;}
    pipe->readers = 1;
    pipe->writers = 1;

    // Read end (flags=0) and write end (flags=1): two open file descriptions,
    // each published complete with its status flags and FD_CLOEXEC (L-59).
    // Both ends are published together or not at all: with one free slot
    // (EMFILE storms) nothing becomes visible to a sibling dup/fork/close,
    // and the unpublished buffer is simply destroyed.
    const uint8_t cloexec = (flags & 0x80000) ? 1 : 0;
    FdEntry readEnd{};readEnd.type = FdType::Pipe;readEnd.handle = pipe;readEnd.flags = 0;readEnd.fdFlags = cloexec;
    FdEntry writeEnd = readEnd;writeEnd.flags = 1;
    int ends[2] = {-1, -1};
    int published = FdPublishNewPair(proc, readEnd, static_cast<uint32_t>(flags & 0x800),
                                     writeEnd, static_cast<uint32_t>(1 | (flags & (0x800 | 0x4000))), ends);
    if (published < 0) { PipeBufferDestroy(pipe); return published; }
    const int readFd = ends[0], writeFd = ends[1];

    const int32_t result[2]={readFd,writeFd};
    if(!CopyToUser(pipefdAddr,result,sizeof(result))) {
        CloseProcessFd(proc,readFd);CloseProcessFd(proc,writeFd);return -EFAULT;
    }

    DbgPrintf("sys_pipe: fd[%d,%d] for pid %u\n", readFd, writeFd, proc->pid);
    return 0;
}

int64_t sys_pipe(uint64_t address,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)
{
    return CreatePipe(address,0);
}

// ---------------------------------------------------------------------------
// sys_pipe2 (293) — like pipe() but with flags
// ---------------------------------------------------------------------------

int64_t sys_pipe2(uint64_t pipefdAddr, uint64_t flags, uint64_t,
                   uint64_t, uint64_t, uint64_t)
{
    constexpr uint64_t PIPE2_ALLOWED = O_CLOEXEC | O_NONBLOCK | 0x4000;
    if (flags & ~PIPE2_ALLOWED) return -EINVAL;
    return CreatePipe(pipefdAddr,flags);
}

} // namespace brook
