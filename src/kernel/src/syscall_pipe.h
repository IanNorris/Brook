#pragma once

// syscall_pipe.h -- internal declarations shared from syscall_pipe.cpp.
// Internal to the syscall layer; split from syscall.cpp.

#include "mount_namespace.h"

#include "pipe.h"
#include "syscall_internal.h"

namespace brook {

#include "pipe_poll_types.inc"
#include "named_fifo_types.inc"

static inline void PipeWakeEpollRaw(PipeBuffer* pipe)
{
    if (!pipe) return;
    if (pipe->terminalReader) WakeProcess(pipe->terminalReader);
    Process* ew = pipe->epollWaiter;
    if (ew) {
        pipe->epollWaiter = nullptr;
        WakeProcess(ew);
    }
}

int NamedFifoAttach(Vnode* node,uint32_t flags);
PipeBuffer* FdPipe(const FdEntry* entry);
NamedFifoOpen* FdNamedFifo(const FdEntry* entry);
bool FdPipeRead(const FdEntry* entry);
uint32_t NamedFifoReady(const FdEntry* entry,uint32_t events);

// Wake any process blocked inside epoll_wait on this pipe. Called from
// sys_write / sys_sendmsg / sys_close after mutating pipe state in a way
// that could transition a watched fd to ready.
static inline void PipeWakeEpoll(PipeBuffer* pipe)
{
    if (!pipe) return;
    SpinLockAcquire(&pipe->lock);PipeWakeEpollRaw(pipe);SpinLockRelease(&pipe->lock);
    PipePollNotify(pipe, 1 | 4);
}

} // namespace brook
