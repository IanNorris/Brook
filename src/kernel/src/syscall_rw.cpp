#include "keyboard_layout.h"
// syscall_rw.cpp -- read/write family: read/write/readv/writev/pread64/pwrite64/sendfile.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "kprintf.h"

#include "signal_fd.h"
#include "user_access_scope.h"
#include "tty.h"
#include "compositor.h"
#include "window.h"
#include "net.h"
#include "tsc_clock.h"
#include "dsp_write_plan.h"
#include "dsp_producer_timing.h"
#include "serial_writer.h"
#include "debug_overlay.h"
#include "syscall_internal.h"
#include "syscall_eventfd.h"
#include "syscall_timerfd.h"
#include "syscall_memfd.h"
#include "syscall_dsp.h"
#include "syscall_pipe.h"
#include "syscall_unix_socket.h"
#include "syscall_maintenance_block.h"
#include "syscall_time.h"
#include "syscall_poll.h"
#include "syscall_rw.h"
#include "syscall_netlink.h"
#include "syscall_socket.h"
#include "syscall_handlers.h"

namespace brook {

// Descriptor numbers are reusable: redirected standard streams have the
// same metadata and terminal behavior as the object they now reference.
bool IsTerminalFd(const FdEntry* fde)
{
    return fde->type == FdType::DevKeyboard ||
           (fde->type == FdType::Vnode && !fde->handle) ||
           fde->type == FdType::DevTty ||
           (fde->type == FdType::Pipe && (fde->flags & 0x04));
}

// Shared write dispatch used by sys_write (user buffer) and sys_sendfile
// (kernel bounce buffer). bufAddr is treated as a raw readable byte pointer;
// the CALLER is responsible for validating it (sys_write checks the user
// buffer; sendfile passes a kernel bounce buffer that is always readable).
// Handles every fd type: serial, vnode, framebuffer, /dev/null, dsp, tty,
// pipe, eventfd, socket, memfd, unix-socket.
// One append syscall commits at most this prefix. A short write permits callers
// to retry without holding inode/backend locks across user faults or allocation.
static constexpr uint64_t AppendStageLimit = 64 * 1024;
static int64_t AppendUserBuffer(Vnode* vn, uint64_t address, uint64_t count, uint64_t* offset)
{
    if (!count) return 0;
    uint64_t size = count < AppendStageLimit ? count : AppendStageLimit;
    auto* staged = kmalloc(size);
    if (!staged) return -ENOMEM;
    const uint64_t copied=CopyFromUserBytes(staged,address,size);
    if(!copied) { kfree(staged);return -EFAULT; }
    int64_t result = VfsAppend(vn, staged, copied, offset);
    kfree(staged);
    return result;
}

// Copy one chunk of a write() source into kernel memory before it reaches a
// backend that copies under a spinlock (serial, pipe rings). Kernel sources
// (sendfile's bounce buffer) are used in place. Returns the bytes staged,
// 0 on a fault.
static uint32_t StageWriteChunk(char* stage, uint32_t capacity, uint64_t address,
                                uint64_t remaining, bool userSource, const char** chunk)
{
    const uint32_t n = static_cast<uint32_t>(remaining < capacity ? remaining : capacity);
    if (!userSource) { *chunk = reinterpret_cast<const char*>(address); return n; }
    if (!CopyFromUser(stage, address, n)) return 0;
    *chunk = stage;
    return n;
}

static int64_t WriteToPinnedFdByType(Process* proc,FdEntry* fde,uint64_t fd,
                                     uint64_t bufAddr,uint64_t count,bool userSource)
{
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)
    if(fde->type==FdType::BlockDevice && fde->handle) {
        if((FdStatus(fde)&3)==0)return -EBADF;
        auto* block=static_cast<MaintenanceBlockFd*>(fde->handle);
        KMutexLock(&block->lock);
        int64_t result=MaintenanceBlockTransfer(block,bufAddr,count,&block->position,true,userSource);
        if(result>0 && (FdStatus(fde)&0x1000)) {const int error=DeviceFlush(block->device);if(error)result=error;}
        KMutexUnlock(&block->lock);return result;
    }
#endif
    if(fde->type==FdType::NetlinkSocket)
        return NetlinkSend(static_cast<NetlinkSocket*>(fde->handle),bufAddr,count,0,0,0,userSource);
    // Debug serial writes directly, bypassing the async ring buffer.
    // Hold the serial lock across the entire write so multi-CPU output
    // doesn't interleave character-by-character.
    if (fde->type == FdType::DebugConsole)
    {
        // Explicit opt-in; ordinary stdout and binary pipelines remain untouched.
        const char* source=reinterpret_cast<const char*>(bufAddr);
        char text[257];
        for(uint64_t offset=0;offset<count;) {
            uint32_t n=static_cast<uint32_t>(count-offset>256?256:count-offset);
            if(userSource) {
                n=static_cast<uint32_t>(CopyFromUserBytes(text,bufAddr+offset,n));
                if(!n)return offset?static_cast<int64_t>(offset):-EFAULT;
            } else memcpy(text,source+offset,n);
            for(uint32_t i=0;i<n;++i)if(!text[i])text[i]='?';
            text[n]=0;brook::KPuts(text);offset+=n;
        }
        return static_cast<int64_t>(count);
    }
    if (fde->type == FdType::DebugSerial)
    {
        // Stage each chunk before taking the serial lock.
        char stage[512];
        for (uint64_t offset = 0; offset < count;)
        {
            const char* buf = nullptr;
            const uint32_t n = StageWriteChunk(stage, sizeof(stage), bufAddr + offset, count - offset, userSource, &buf);
            if (!n) return offset ? static_cast<int64_t>(offset) : -EFAULT;
            brook::SerialLock();
            for (uint32_t i = 0; i < n; i++)
            {
                if (buf[i]) // skip null bytes (binary padding in buffers)
                    brook::SerialPutChar(buf[i]);
            }
            brook::SerialUnlock();
            offset += n;
        }
        return static_cast<int64_t>(count);
    }

    // For fd 0/1/2: check if they've been redirected via dup2 first
    if (fd == 1 || fd == 2)
    {
        // Redirected if FD entry is a pipe or other non-default type.
        // Default: Vnode with null handle = serial output
        if (fde->type == FdType::Vnode && !fde->handle)
        {
            // The serial writer copies into its queue; give it kernel bytes.
            char stage[4096];
            for (uint64_t offset = 0; offset < count;)
            {
                const char* buf = nullptr;
                const uint32_t chunk = StageWriteChunk(stage, sizeof(stage), bufAddr + offset, count - offset, userSource, &buf);
                if (!chunk) return offset ? static_cast<int64_t>(offset) : -EFAULT;
                brook::SerialWriterEnqueue(buf, chunk);
                offset += chunk;
            }
            return static_cast<int64_t>(count);
        }
        // Fall through to FD-table-based write below. (Historically we
        // mirrored redirected stdout/stderr to the serial log here, but
        // that corrupts binary pipelines — e.g. curl | xz | nar-unpack
        // dumps the compressed NAR stream onto the serial console — and
        // makes debug output confusing. If you need pipe content in logs,
        // explicitly tee to the inherited debug-serial descriptor.)
    }

    if(auto* opened=FdNamedFifo(fde)) {
        if((FdStatus(fde)&3)==0)return -EBADF;
        FdEntry endpoint=*fde;endpoint.type=FdType::Pipe;endpoint.handle=opened->state->pipe;endpoint.flags=1|0x10;
        return WriteToPinnedFdByType(proc,&endpoint,fd,bufAddr,count,userSource);
    }
    // The pin above also protects handles across blocking writes; its
    // destructor performs any deferred-close teardown on every return path.
    if (fde->type == FdType::Vnode && fde->handle)
    {
        if ((FdStatus(fde) & 3) == 0) return -EBADF;
        auto* vn = static_cast<Vnode*>(fde->handle);
        // The offset is shared by every dup/fork alias (H-20); hold the
        // description's position across the backend update. O_APPEND is
        // additionally atomic against other descriptions in the backend.
        const uint32_t status = FdStatus(fde);
        FdPosition position(fde);
        int64_t ret = (status & 0x400)
            ? AppendUserBuffer(vn, bufAddr, count, position.get())
            : VfsWrite(vn, reinterpret_cast<const void*>(bufAddr),
                       count < VnodeWriteLimit ? count : VnodeWriteLimit, position.get());
        // O_SYNC includes O_DSYNC (0x1000). A stronger full-file sync is valid
        // for either flag and must complete before reporting a durable write.
        if (ret > 0 && (FdStatus(fde) & 0x1000)) {
            int error = VfsFsync(vn);
            if (error) return error;
        }
        // Note: VfsWrite already updates *offset (the shared position)
        return ret;
    }

    // Write to /dev/fb0 signals a frame is complete (sets dirty flag).
    if (fde->type == FdType::DevFramebuf)
    {
        ProcessFramebufferMarkDirty(proc);
        CompositorWake();
        return static_cast<int64_t>(count);
    }

    // /dev/null — discard all writes
    if (fde->type == FdType::DevNull || fde->type == FdType::DevUrandom ||
        fde->type == FdType::DevZero)
        return static_cast<int64_t>(count);
    if (fde->type == FdType::DevFull)
        return -28; // ENOSPC

    // /dev/dsp — pass PCM data through the audio mixer.
    // All streams are resampled to 44100 Hz stereo 16-bit, then submitted
    // to the mixer which accumulates and flushes to hardware.
    // Block when the mixer ring is full so we apply backpressure instead
    // of silently dropping samples (which causes audible stutter/gaps).
    if (fde->type == FdType::DevDsp && fde->handle)
    {
        auto* dsp = static_cast<DspState*>(fde->handle);
        DspWritePlan plan;
        DspProducerCall timing(proc->pid,proc->incarnation,dsp->mixerStreamId,count);
        auto finish = [&](int64_t error) -> int64_t {
            const uint64_t bytes = plan.ConsumedBytes();
            return timing.Result(bytes ? static_cast<int64_t>(bytes) : error);
        };
        for (;;) {
            SpinLockAcquire(ProcessFdLock(proc));
            const bool nonblock = (FdStatus(fde) & 0x800) != 0;
            SpinLockRelease(ProcessFdLock(proc));
            {
                const auto mutexBefore=timing.Mark();
                DspIoGuard held(dsp);timing.Mutex(mutexBefore);
                AudioStreamProgress progress{};
                if (!AudioMixerGetProgress(dsp->mixerStreamId, &progress)) return finish(-EBADF);
                if (progress.discontinuity) {
                    // Return the already accepted prefix before crossing an
                    // epoch boundary. A later write retries only unconsumed data.
                    if(plan.Accepted())return finish(0);
                    if(!DspRecoverStreamLocked(dsp,progress))return finish(-EBADF);
                }
                const uint32_t rate = dsp->sampleRate ? dsp->sampleRate : DSP_HW_RATE;
                const uint32_t frameBytes = (dsp->bitsPerSample / 8) * dsp->channels;
                if (!frameBytes) return finish(-EINVAL);
                if (!plan.Active() || !plan.Matches(rate, dsp->channels, dsp->bitsPerSample,
                                                   dsp->controlEpoch, progress.epoch)) {
                    // A reset/pause/reconfiguration cannot splice the rest of an
                    // already accepted chunk into a different stream epoch.
                    if (plan.Accepted()) return finish(0);
                    if (count / frameBytes == 0) return timing.Result(0);
                    if (!plan.Begin(count, rate, dsp->channels, dsp->bitsPerSample,
                                    progress.capacityFrames, dsp->controlEpoch, progress.epoch))
                        return timing.Result(-EINVAL);
                }
                const uint32_t offered = plan.Offer(AudioMixerAvailableFrames(dsp->mixerStreamId));
                if (offered) {
                    // Re-stage under ioLock on each attempt. Nothing aliases
                    // these shared buffers across a wait or another writer.
                    // Conversion keeps its original output offset even when
                    // the hardware rate differs from the input rate.
                    if (userSource) {
                        if (!CopyFromUser(dsp->inputBuffer, bufAddr, plan.InputBytes())) return finish(-EFAULT);
                    } else {
                        memcpy(dsp->inputBuffer, reinterpret_cast<const void*>(bufAddr), plan.InputBytes());
                    }
                    auto* out = reinterpret_cast<int16_t*>(dsp->buffer);
                    if (!plan.Convert(dsp->inputBuffer, out, offered)) return finish(-EIO);
                    const uint32_t accepted = AudioMixerSubmit(out, offered, dsp->mixerStreamId);
                    if (!plan.Accept(accepted)) return finish(-EIO);
                    if (accepted) {
                        // Preserve the existing bounded full-write contract for
                        // blocking users, but refill available prefixes instead
                        // of waiting for the entire converted chunk to fit.
                        if (!plan.Remaining() || nonblock) return finish(0);
                        continue;
                    }
                }
            }
            // The description mutex is released while waiting, so a separate
            // thread can reset, pause or enable this same open description.
            // Prefix acceptance wins over a later signal/error, as for write(2).
            if (nonblock) return finish(-EAGAIN);
            if (HasReadInterruptSignal()) return finish(-EINTR);
            Process* self = SchedulerCurrentProcess();
            if (self) {
                const auto before=timing.Mark();
                self->wakeupTick = ApicTickCount() + 5; SchedulerBlockInterruptible(self);
                timing.Sleep(before);
            }
        }
    }

    // Write to /dev/tty (DevKeyboard) — route to serial + TTY framebuffer
    if (fde->type == FdType::DevKeyboard)
    {
        char stage[512];
        for (uint64_t offset = 0; offset < count;)
        {
            const char* buf = nullptr;
            const uint32_t n = StageWriteChunk(stage, sizeof(stage), bufAddr + offset, count - offset, userSource, &buf);
            if (!n) return offset ? static_cast<int64_t>(offset) : -EFAULT;
            brook::SerialLock();
            for (uint32_t i = 0; i < n; i++)
                brook::SerialPutChar(buf[i]);
            brook::SerialUnlock();
            for (uint32_t i = 0; i < n; i++)
                TtyPutChar(buf[i]);
            offset += n;
        }
        return static_cast<int64_t>(count);
    }

#include "pipe_write.inc"

    // Write to eventfd — adds value to counter
    if (fde->type == FdType::SignalFd) return -EINVAL;
    if (fde->type == FdType::SyncFile) return -EBADF;

    if (fde->type == FdType::EventFd && fde->handle)
    {
        if (count != 8) return -EINVAL;
        uint64_t val;
        if (!CopyFromUser(&val, bufAddr, sizeof(val))) return -EFAULT;
        if (val == 0xFFFFFFFFFFFFFFFFULL) return -EINVAL;
        auto* efd = static_cast<EventFdData*>(fde->handle);
        EventFdWaitGroup waiters;
        bool registered = false;
        for (;;) {
            bool accepted = false;
            {
                EventFdGuard guard(efd);
                if (val <= (~0ULL - 1) - efd->counter) {
                    efd->counter += val;
                    ++efd->inGeneration; // every write, including zero
                    accepted = true;
                    // Every blocked reader, poller and epoll waiter wakes.
                    EventFdWakeLocked(efd);
                }
            }
            if (accepted) break;
            if (FdStatus(fde) & EFD_NONBLOCK) return -EAGAIN;
            // Register, then recheck before blocking: a read that frees
            // capacity in between wakes this writer (or leaves the wake
            // pending). The timeout only covers an allocation failure.
            Process* self = ProcessCurrent();
            if (!registered && (registered = waiters.add(efd, self))) continue;
            self->wakeupTick = ApicTickCount() + (registered ? 100 : 10);
            SchedulerBlockInterruptible(self);
            if (HasPendingSignals()) return -EINTR;
        }
        return 8;
    }

    // Write to socket (TCP stream or connected UDP datagram).
    if (fde->type == FdType::Socket && fde->handle)
    {
        int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
        if (brook::SockIsStream(sockIdx))
        {
            const uint32_t bytes = static_cast<uint32_t>(count > 0x7ffff000ULL ? 0x7ffff000ULL : count);
            return userSource ? brook::SockSendUser(sockIdx, bufAddr, bytes)
                              : brook::SockSend(sockIdx, reinterpret_cast<const void*>(bufAddr), bytes);
        }
        // UDP: write() requires a prior connect() so the kernel knows the peer.
        // SockSendTo with dest=nullptr falls back to the cached connect address.
        int ret = SockSendTo(sockIdx,
                             reinterpret_cast<const void*>(bufAddr),
                             static_cast<uint32_t>(count),
                             nullptr);
        if (ret < 0) return -ENOTCONN;
        return static_cast<int64_t>(count);
    }

    // Write to /dev/tty — writes to stdout pipe (rendered by terminal thread)
    if (fde->type == FdType::DevTty && fde->handle)
    {
        auto* pair = static_cast<TtyDevicePair*>(fde->handle);
        auto* pipe = static_cast<PipeBuffer*>(pair->writePipe);
        uint64_t written = 0;
        char stage[4096];

        while (written < count)
        {
            if (__atomic_load_n(&pipe->readers, __ATOMIC_ACQUIRE) == 0)
                return written > 0 ? static_cast<int64_t>(written) : -EPIPE;

            // The ring copy runs under pipe->lock; stage the chunk first.
            const char* staged = nullptr;
            const uint32_t want = StageWriteChunk(stage, sizeof(stage), bufAddr + written, count - written, userSource, &staged);
            if (!want) return written > 0 ? static_cast<int64_t>(written) : -EFAULT;
            uint32_t chunk = pipe->write(staged, want);
            written += chunk;
            if (written > 0)
            {
                Process* reader = pipe->readerWaiter;
                if (reader)
                {
                    pipe->readerWaiter = nullptr;
                    WakeProcess(reader);
                }
                PipeWakeEpoll(pipe);
                break;
            }
            Process* self = ProcessCurrent();
            pipe->writerWaiter = self;
            // Re-check after registering waiter
            chunk = pipe->write(staged, want);
            if (chunk > 0)
            {
                pipe->writerWaiter = nullptr;
                written += chunk;
                Process* reader = pipe->readerWaiter;
                if (reader) { pipe->readerWaiter = nullptr; WakeProcess(reader); }
                PipeWakeEpoll(pipe);
                break;
            }
            self->wakeupTick = ApicTickCount() + 10;
            SchedulerBlock(self);
            if (HasPendingSignals())
                return written > 0 ? static_cast<int64_t>(written) : -EINTR;
        }
        return static_cast<int64_t>(written);
    }

    if (fde->type == FdType::MemFd && fde->handle) {
        if ((FdStatus(fde) & 3) == 0) return -EBADF;
        const bool append = (FdStatus(fde) & 0x400) != 0;
        FdPosition position(fde);
        return MemFdWriteAt(static_cast<MemFdData*>(fde->handle),
            bufAddr, count, position.get(), append, userSource);
    }

    // Write to AF_UNIX connected socket.
    if (fde->type == FdType::UnixSocket && fde->handle) {
        auto* socket=static_cast<UnixSocketData*>(fde->handle);
        return UnixSocketWriteSome(socket,(FdStatus(fde)&0x800)!=0,
            reinterpret_cast<const char*>(bufAddr),count);
    }

    // No fd-type handler above matched — unknown/unsupported descriptor.
    // (This is the terminal return of WriteToPinnedFdByType, not sys_write.)
    return -EBADF;
}

static int64_t WriteToFdByType(uint64_t fd,uint64_t bufAddr,uint64_t count,bool userSource=true)
{
    Process* proc=ProcessCurrent();if(!proc)return -EBADF;
    FdRef ref(proc,static_cast<int>(fd));if(!ref)return -EBADF;
    return WriteToPinnedFdByType(proc,ref.get(),fd,bufAddr,count,userSource);
}

int64_t sys_write(uint64_t fd, uint64_t bufAddr, uint64_t count,
                   uint64_t, uint64_t, uint64_t)
{
    UserAccessScope access(ProcessCurrent());
    // Validate the user buffer once, up front. EFAULT if any page in the
    // range is unmapped — without this, a bad user pointer faults the kernel.
    if (count > 0 && !UserBufferReadable(bufAddr, count))
        return -EFAULT;
    return WriteToFdByType(fd, bufAddr, count);
}

// ---------------------------------------------------------------------------
// sys_read (0)
// ---------------------------------------------------------------------------

// SockRecv holds a socket spinlock while copying its receive ring. Never pass
// a userspace pointer: even a previously validated writable page may need CoW
// or be remapped before that copy, and fault resolution can sleep on mapping
// mutexes. Stage resident kernel bytes, then copy out after the socket unlocks.
int64_t ReceiveTcpToUser(int sockIdx, uint64_t address, uint64_t count, bool dontwait)
{
    if (!count) return 0;
    const uint32_t size = static_cast<uint32_t>(count < 65536 ? count : 65536);
    if (!UserBufferWritable(address, size)) return -EFAULT;
    KernelOperationGuard operation(ProcessCurrent());
    auto* staged = static_cast<uint8_t*>(kmalloc(size));
    if (!staged) return -ENOMEM;
    int result = brook::SockRecv(sockIdx, staged, size, dontwait);
    if (result > 0 && !CopyToUser(address, staged, static_cast<uint32_t>(result)))
        result = -EFAULT;
    kfree(staged);
    return result;
}

int64_t ReceiveUdpToUser(int sockIdx, uint64_t address, uint64_t count, SockAddrIn* source)
{
    // A UDP datagram is at most 64 KiB; longer reads cannot receive more.
    const uint32_t size = static_cast<uint32_t>(count < 65536 ? count : 65536);
    if (size && !UserBufferWritable(address, size)) return -EFAULT;
    auto* staged = static_cast<uint8_t*>(kmalloc(size ? size : 1));
    if (!staged) return -ENOMEM;
    int result = SockRecvFrom(sockIdx, staged, size, source);
    if (result > 0 && !CopyToUser(address, staged, static_cast<uint32_t>(result)))
        result = -EFAULT;
    kfree(staged);
    return result;
}

int64_t CopySockAddrToUser(uint64_t address, uint64_t addrLen, const void* value, uint32_t actual)
{
    uint32_t requested = 0;
    if (!CopyFromUser(&requested, addrLen, sizeof(requested))) return -EFAULT;
    if (requested > 0x7fffffffu) return -EINVAL;
    const uint32_t copied = requested < actual ? requested : actual;
    if ((copied && !CopyToUser(address, value, copied)) ||
        !CopyToUser(addrLen, &actual, sizeof(actual))) return -EFAULT;
    return 0;
}

static int64_t PipeReadSome(Process* proc,FdEntry* fde,char* dst,uint64_t count)
{
    KernelOperationGuard operation(proc);
    if(!count)return 0;
    if(!FdPipeRead(fde))return -EBADF;
    auto* pipe=FdPipe(fde);
        for (;;)
        {
            uint32_t got = pipe->read(dst, static_cast<uint32_t>(count));
            if (got > 0)
            {
                // Wake blocked writer
                SpinLockAcquire(&pipe->lock);
                Process* writer=pipe->writerWaiter;pipe->writerWaiter=nullptr;
                if(writer)WakeProcess(writer);
                SpinLockRelease(&pipe->lock);
                return static_cast<int64_t>(got);
            }

            Process* self = ProcessCurrent();
            switch (pipe->prepareReadWait(self, (FdStatus(fde) & 0x800) != 0)) {
                case PipeReadWait::Retry: continue;
                case PipeReadWait::Eof: return 0;
                case PipeReadWait::WouldBlock: return -EAGAIN;
                case PipeReadWait::Registered: break;
            }
            // Use timed wakeup to recheck writer count periodically
            // (avoids permanent deadlock if close notification was missed)
            self->wakeupTick = ApicTickCount() + 10; // recheck every ~10ms
            SchedulerBlock(self);
            pipe->removeReadWait(self);
            if (HasReadInterruptSignal()) {
                return -EINTR;
            }
        }
}

// Bytes a device read produces are collected in kernel memory and flushed
// with the checked user copy, so a raced unmap returns -EFAULT (or the bytes
// already delivered) instead of faulting the kernel.
struct ReadStage {
    uint64_t address;
    uint64_t done = 0;
    uint32_t used = 0;
    bool failed = false;
    uint8_t bytes[256];
    explicit ReadStage(uint64_t destination) : address(destination) {}
    bool Put(uint8_t value) {
        if (failed) return false;
        bytes[used++] = value;
        return used < sizeof(bytes) || Flush();
    }
    bool Flush() {
        if (failed) return false;
        if (!used) return true;
        if (!CopyToUser(address + done, bytes, used)) { failed = true; return false; }
        done += used; used = 0;
        return true;
    }
    uint64_t Total() const { return done + used; }
    int64_t Result() {
        Flush();
        return failed && !done ? -EFAULT : static_cast<int64_t>(done);
    }
};

int64_t sys_read(uint64_t fd, uint64_t bufAddr, uint64_t count,
                  uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    UserAccessScope access(proc);
    if (!proc) return -EBADF;
    // Pin the slot for the whole operation: the pipe/socket/tty/timerfd/eventfd
    // paths below block while holding fde->handle, so a bare FdGet would expose
    // a use-after-free if a sibling closes the fd (BRO-156). The guard's
    // destructor runs FdPut on every return path.
    FdRef ref(proc, static_cast<int>(fd));
    if (!ref) return -EBADF;
    FdEntry* fde = ref.get();

    // Validate user buffer up-front — every read path below ultimately
    // writes to bufAddr. If the user munmapped or never mapped the range,
    // a kernel #PF would be unrecoverable. Resolve CoW and verify write access
    // before backend locks; the lexical scope excludes pager eviction through
    // a blocking read, but does not make raced unmap/protection changes safe.
    if (count > 0 && !UserBufferWritable(bufAddr, count))
        return -EFAULT;

#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)
    if(fde->type==FdType::BlockDevice && fde->handle) {
        if((FdStatus(fde)&3)==1)return -EBADF;
        auto* block=static_cast<MaintenanceBlockFd*>(fde->handle);
        KMutexLock(&block->lock);
        const int64_t result=MaintenanceBlockTransfer(block,bufAddr,count,&block->position,false,true);
        KMutexUnlock(&block->lock);return result;
    }
#endif
    if (fde->type == FdType::Vnode && fde->handle && !FdNamedFifo(fde))
    {
        // A write-only descriptor cannot read, whatever the file mode.
        if (!FdOpenForReading(fde)) return -EBADF;
        auto* vn = static_cast<Vnode*>(fde->handle);
        FdPosition position(fde);
        int64_t ret = VfsRead(vn, reinterpret_cast<void*>(bufAddr),
                              static_cast<uint32_t>(count), position.get());
        // VfsRead already updates the shared offset through the pointer.
        return ret;
    }

    // /dev/null — always EOF
    if (fde->type == FdType::DevNull)
        return 0;

    // /dev/zero, /dev/full — zero-filled reads
    if (fde->type == FdType::DevZero || fde->type == FdType::DevFull)
    {
        if (count > 0x7ffff000) count = 0x7ffff000;
        ReadStage out(bufAddr);
        for (uint64_t i = 0; i < count; ++i)
            if (!out.Put(0)) return out.Result();
        return out.Result();
    }

    // /dev/urandom — fill buffer with RDRAND random bytes
    if (fde->type == FdType::DevUrandom)
    {
        ReadStage out(bufAddr);
        uint64_t filled = 0;
        while (filled < count) {
            uint64_t rval;
            if (!RdrandU64(&rval))
                rval = SoftRandU64();
            uint64_t remain = count - filled;
            uint64_t chunk = (remain < 8) ? remain : 8;
            for (uint64_t i = 0; i < chunk; i++)
                if (!out.Put(static_cast<uint8_t>(rval >> (i * 8)))) return out.Result();
            filled += chunk;
        }
        return out.Result();
    }

    // /dev/klog — read new lines from the kernel log ring buffer
    if (fde->type == FdType::DevKlog && fde->handle)
    {
        auto* cursor = static_cast<uint32_t*>(fde->handle);
        ReadStage out(bufAddr);

        // Read available lines from the ring buffer into a temp buffer
        static constexpr uint32_t KLOG_LINE_LEN = 161; // 160 chars + null
        static constexpr uint32_t KLOG_BATCH    = 8;  // 8 × 161 = ~1.3KB stack
        char lineBuf[KLOG_BATCH][KLOG_LINE_LEN];

        for (;;) {
            uint32_t nread = DebugOverlayReadFrom(cursor, reinterpret_cast<char*>(lineBuf),
                                                   KLOG_BATCH, KLOG_LINE_LEN);
            if (nread == 0) break;

            for (uint32_t i = 0; i < nread; i++) {
                uint32_t len = 0;
                while (len < KLOG_LINE_LEN - 1 && lineBuf[i][len]) len++;

                // Need space for line + newline
                if (out.Total() + len + 1 > count) goto done;

                for (uint32_t j = 0; j < len; j++)
                    if (!out.Put(static_cast<uint8_t>(lineBuf[i][j]))) return out.Result();
                if (!out.Put('\n')) return out.Result();
            }
        }

    done:
        if (out.Total() == 0 && !(FdStatus(fde) & 0x800)) { // O_NONBLOCK
            // Blocking mode: sleep briefly and retry once
            Process* self = ProcessCurrent();
            self->wakeupTick = ApicTickCount() + 100;
            SchedulerBlock(self);

            uint32_t nread = DebugOverlayReadFrom(cursor, reinterpret_cast<char*>(lineBuf),
                                                   KLOG_BATCH, KLOG_LINE_LEN);
            for (uint32_t i = 0; i < nread; i++) {
                uint32_t len = 0;
                while (len < KLOG_LINE_LEN - 1 && lineBuf[i][len]) len++;
                if (out.Total() + len + 1 > count) break;
                for (uint32_t j = 0; j < len; j++)
                    if (!out.Put(static_cast<uint8_t>(lineBuf[i][j]))) return out.Result();
                if (!out.Put('\n')) return out.Result();
            }
        }

        return out.Result();
    }

    // Synthetic in-memory files (/etc/passwd, /etc/group, etc.)
    if (fde->type == FdType::SyntheticMem && fde->handle)
    {
        auto* content = static_cast<const char*>(fde->handle);
        uint64_t contentLen = 0;
        while (content[contentLen]) contentLen++;

        FdPosition position(fde);
        uint64_t pos = position.value();
        if (pos >= contentLen) return 0; // EOF

        uint64_t avail = contentLen - pos;
        uint64_t toRead = (count < avail) ? count : avail;
        if (!CopyToUser(bufAddr, content + pos, toRead)) return -EFAULT;
        position.set(pos + toRead);
        return static_cast<int64_t>(toRead);
    }

    if (fde->type == FdType::DevKeyboard)
    {
        // read(fd, buf, 0) neither blocks nor consumes input. The buffer is
        // unchecked for count 0, so nothing below may store through it.
        if (count == 0) return 0;
        ReadStage out(bufAddr);

        // Non-blocking raw scancode mode (DOOM uses ioctl cmd=1 to enable this)
        if (fde->flags & 1)
        {
            while (out.Total() < count)
            {
                InputEvent ev{};
                bool got;
                if (WmIsActive())
                    got = ProcessInputPop(proc, &ev);
                else
                    got = InputPollEvent(&ev);
                if (!got) break;

                uint8_t sc = ev.scanCode;
                bool release = (ev.type == InputEventType::KeyRelease);

                // Translate synthetic extended codes back to PS/2 E0-prefixed
                // two-byte sequences so userspace (DOOM, Quake) sees real scancodes.
                uint8_t ps2 = 0;
                switch (sc)
                {
                case SC_EXT_UP:     ps2 = 0x48; break;
                case SC_EXT_DOWN:   ps2 = 0x50; break;
                case SC_EXT_LEFT:   ps2 = 0x4B; break;
                case SC_EXT_RIGHT:  ps2 = 0x4D; break;
                case SC_EXT_HOME:   ps2 = 0x47; break;
                case SC_EXT_END:    ps2 = 0x4F; break;
                case SC_EXT_INSERT: ps2 = 0x52; break;
                case SC_EXT_DELETE: ps2 = 0x53; break;
                case SC_EXT_PGUP:   ps2 = 0x49; break;
                case SC_EXT_PGDN:   ps2 = 0x51; break;
                }

                bool stored;
                if (ps2)
                {
                    // Emit single-byte scancode (no E0 prefix) for compatibility
                    // with apps that read 1 byte at a time (DOOM, etc.)
                    stored = out.Put(release ? (ps2 | 0x80) : ps2);
                }
                else
                {
                    if (release) sc |= 0x80;
                    stored = out.Put(sc);
                }
                if (!stored) break;
            }
            return out.Result();
        }

        // Non-canonical (raw) terminal mode: return ASCII chars as they arrive.
        // Shells like ash use this for line editing.
        if (!proc->ttyCanonical)
        {
            bool wmMode = WmIsActive();
            while (out.Total() < count)
            {
                InputEvent ev{};
                bool got = wmMode ? ProcessInputPop(proc, &ev)
                                  : InputPollEvent(&ev);
                if (!got)
                {
                    if (out.Total() > 0) break; // return what we have
                    // Register as waiter BEFORE re-checking, to close the
                    // race where a key arrives between the poll and block.
                    InputAddWaiter(proc);
                    // Re-check after registration — if data arrived between
                    // the first poll and AddWaiter, consume it immediately.
                    got = wmMode ? ProcessInputPop(proc, &ev)
                                : InputPollEvent(&ev);
                    if (got)
                    {
                        InputRemoveWaiter(proc);
                        goto got_event_nc;
                    }
                    SchedulerBlock(proc);
                    if (HasPendingSignals())
                        return out.Total() > 0 ? out.Result() : -EINTR;
                    continue;
                }
            got_event_nc:
                if (ev.type != InputEventType::KeyPress) continue;
                char c = ev.ascii;
                if (c == 0) continue; // non-printable (arrows, etc.) — skip

                // Map Enter scancode to '\n'
                if (ev.scanCode == 0x1C) c = '\n';

                // Ctrl+C → send interrupt character
                if ((ev.modifiers & INPUT_MOD_CTRL) && (ev.scanCode == 0x2E))
                    c = '\x03';

                // Ctrl+D → EOF
                if ((ev.modifiers & INPUT_MOD_CTRL) && (ev.scanCode == 0x20))
                {
                    if (out.Total() == 0) return 0; // EOF
                    break;
                }

                if (!out.Put(static_cast<uint8_t>(c))) break;

                // Don't echo here — bash handles its own echo in
                // non-canonical mode by redisplaying the line on Enter.
                // Kernel echo would duplicate every character.
            }
            return out.Result();
        }

        // Cooked terminal mode: blocking, ASCII translation, line buffering, echo.
        // Buffer a full line (until Enter), then return it.
        // This matches canonical terminal behavior that shells expect.
        static constexpr uint32_t LINE_BUF_MAX = 256;
        char lineBuf[LINE_BUF_MAX];
        uint32_t lineLen = 0;
        bool wmMode = WmIsActive();

        for (;;)
        {
            InputEvent ev{};
            bool got = wmMode ? ProcessInputPop(proc, &ev)
                              : InputPollEvent(&ev);
            if (!got)
            {
                // Register as waiter BEFORE re-checking to close the race.
                InputAddWaiter(proc);
                got = wmMode ? ProcessInputPop(proc, &ev)
                             : InputPollEvent(&ev);
                if (got)
                {
                    InputRemoveWaiter(proc);
                    goto got_event_cooked;
                }
                SchedulerBlock(proc);
                if (HasPendingSignals())
                    return lineLen > 0 ? static_cast<int64_t>(lineLen) : -EINTR;
                continue;
            }
        got_event_cooked:

            // Only process key presses, ignore releases
            if (ev.type != InputEventType::KeyPress)
                continue;

            char c = ev.ascii;

            // Ctrl+C → interrupt (for now, just send '\x03')
            if ((ev.modifiers & INPUT_MOD_CTRL) && (ev.scanCode == 0x2E)) // 'C'
            {
                // Echo ^C and return empty line
                brook::SerialLock(); brook::SerialPutChar('^'); brook::SerialPutChar('C'); brook::SerialPutChar('\n'); brook::SerialUnlock();
                TtyPutChar('^'); TtyPutChar('C'); TtyPutChar('\n');
                const char newline = '\n';
                return CopyToUser(bufAddr, &newline, 1) ? 1 : -EFAULT;
            }

            // Ctrl+D → EOF
            if ((ev.modifiers & INPUT_MOD_CTRL) && (ev.scanCode == 0x20)) // 'D'
            {
                if (lineLen == 0) return 0; // EOF
                // Otherwise flush current line
                break;
            }

            // Enter/Return
            if (c == '\r' || c == '\n' || ev.scanCode == 0x1C)
            {
                brook::SerialLock(); brook::SerialPutChar('\n'); brook::SerialUnlock();
                TtyPutChar('\n');
                if (lineLen < LINE_BUF_MAX)
                    lineBuf[lineLen++] = '\n';
                break;
            }

            // Backspace
            if (c == '\b' || ev.scanCode == 0x0E)
            {
                if (lineLen > 0)
                {
                    do { --lineLen; } while (lineLen && (uint8_t(lineBuf[lineLen]) & 0xc0) == 0x80);
                    brook::SerialLock(); brook::SerialPutChar('\b'); brook::SerialPutChar(' '); brook::SerialPutChar('\b'); brook::SerialUnlock();
                    TtyPutChar('\b'); TtyPutChar(' '); TtyPutChar('\b');
                }
                continue;
            }

            // Tab → send tab character
            if (ev.scanCode == 0x0F)
            {
                if (lineLen < LINE_BUF_MAX - 1)
                {
                    lineBuf[lineLen++] = '\t';
                    brook::SerialLock(); brook::SerialPutChar('\t'); brook::SerialUnlock();
                    TtyPutChar('\t');
                }
                continue;
            }

            if (ev.textCodepoint >= 128) {
                char bytes[3];
                const unsigned n = KeyboardUtf8(ev.textCodepoint, bytes);
                if (n < LINE_BUF_MAX - lineLen) {
                    brook::SerialLock();
                    for (unsigned i = 0; i < n; ++i) {
                        lineBuf[lineLen++] = bytes[i];
                        brook::SerialPutChar(bytes[i]);
                    }
                    brook::SerialUnlock();
                    TtyPutChar('?'); // The emergency TTY font is ASCII.
                }
                continue;
            }

            // Regular printable character
            if (c >= ' ' && c <= '~')
            {
                if (lineLen < LINE_BUF_MAX - 1)
                {
                    lineBuf[lineLen++] = c;
                    brook::SerialLock(); brook::SerialPutChar(c); brook::SerialUnlock();
                    TtyPutChar(c);
                }
                continue;
            }

            // Ignore non-printable keys (arrows, function keys, etc.)
        }

        // Copy line buffer to user buffer
        uint64_t copyLen = lineLen;
        if (copyLen > count) copyLen = count;
        if (copyLen && !CopyToUser(bufAddr, lineBuf, copyLen)) return -EFAULT;
        return static_cast<int64_t>(copyLen);
    }

    // Read from pipe — block until data available or all writers closed
    if (FdPipe(fde))
    {
        auto* pipe=FdPipe(fde);
        uint64_t capacity=count<pipe->capacity?count:pipe->capacity;
        char small[4096];
        char* staged=capacity>sizeof(small)?static_cast<char*>(kmalloc(capacity)):small;
        if(!staged)return -ENOMEM;
        int64_t got=PipeReadSome(proc,fde,staged,capacity);
        if(got>0) {
            uint64_t copied=CopyToUserBytes(bufAddr,staged,static_cast<uint64_t>(got));
            got=copied?static_cast<int64_t>(copied):-EFAULT;
        }
        if(staged!=small)kfree(staged);
        return got;
    }

    if(fde->type==FdType::SignalFd && fde->handle)
        return SignalFdRead(proc,static_cast<SignalFd*>(fde->handle),bufAddr,count,(FdStatus(fde)&0x800)!=0);

    // Read from eventfd — returns counter as uint64, resets to 0
    if (fde->type == FdType::SyncFile) return -EINVAL;

    if (fde->type == FdType::EventFd && fde->handle)
    {
        if (count < 8) return -EINVAL;
        auto* efd = static_cast<EventFdData*>(fde->handle);
        EventFdWaitGroup waiters;
        bool registered = false;

        for (;;)
        {
            bool consumed = false;
            {
                EventFdGuard guard(efd);
                if (efd->counter > 0) {
                    const bool semaphore = (efd->flags & EFD_SEMAPHORE) != 0;
                    const uint64_t val = semaphore ? 1 : efd->counter;
                    // A sleeping mutex permits fault-aware copy before state
                    // mutation. No spinlock is held across userspace copying.
                    if (!CopyToUser(bufAddr, &val, sizeof(val))) return -EFAULT;
                    efd->counter = semaphore ? efd->counter - 1 : 0;
                    ++efd->outGeneration;
                    consumed = true;
                    // Capacity changed: wake writers and POLLOUT pollers.
                    EventFdWakeLocked(efd);
                }
            }
            if (consumed) return 8;
            // Non-blocking: return EAGAIN
            if (FdStatus(fde) & 0x800)
                return -EAGAIN;

            // Register, recheck, then block until the counter is non-zero.
            // Concurrent readers and pollers each hold their own registration.
            Process* self = ProcessCurrent();
            if (!registered && (registered = waiters.add(efd, self))) continue;
            self->wakeupTick = ApicTickCount() + (registered ? 100 : 10);
            SchedulerBlock(self);
            if (HasPendingSignals())
                return -EINTR;
        }
    }

    if(fde->type==FdType::NetlinkSocket)
        return count?NetlinkReadUser(static_cast<NetlinkSocket*>(fde->handle),bufAddr,count,0):0;

    // Read from socket (TCP stream or connected UDP)
    if (fde->type == FdType::Socket && fde->handle)
    {
        int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
        bool nonblock = brook::SockIsNonblock(sockIdx);
        if (brook::SockIsStream(sockIdx))
        {
            return ReceiveTcpToUser(sockIdx, bufAddr, count, nonblock);
        }
        // UDP: read() pulls one datagram from the rx queue.  SockRecvFrom
        // returns -EAGAIN when empty; non-blocking sockets propagate that,
        // blocking sockets spin-poll briefly (matches existing TCP behaviour).
        SockAddrIn unused = {};
        for (;;) {
            const int64_t ret = ReceiveUdpToUser(sockIdx, bufAddr, count, &unused);
            if (ret >= 0 || ret == -EFAULT || ret == -ENOMEM) return ret;
            if (nonblock) return -EAGAIN;
            if (HasPendingSignals()) return -EINTR;
            // Yield briefly so the NIC ISR / poll path can deliver packets.
            __asm__ volatile("pause");
        }
    }

    // Read from timerfd — returns uint64 expiry count, blocks until armed+expired
    if (fde->type == FdType::TimerFd && fde->handle)
    {
        if (count < 8) return -EINVAL;
        auto* tfd = static_cast<TimerFdData*>(fde->handle);
        bool nonblock = (FdStatus(fde) & 0x800) != 0;

        TimerWaitGroup waiters;
        if (!nonblock) waiters.add(tfd, proc);
        for (;;) {
            {
                TimerFdGuard guard(tfd);
                tfd->state.refresh(ClockNowNs(tfd->clockId));
                if (tfd->state.pending) {
                    const uint64_t total = tfd->state.pending;
                    if (!CopyToUser(bufAddr, &total, sizeof(total))) return -EFAULT;
                    tfd->state.pending = 0;
                    return 8;
                }
            }
            if (nonblock) return -EAGAIN;
            if (HasPendingSignals()) return -EINTR;
            // Assign only after every potentially sleeping timer lock. Pending
            // wakeups close the rearm-between-snapshot-and-block race.
            const uint64_t deadline = TimerFdDeadline(tfd);
            proc->wakeupTick = WaitWakeupTick(deadline, waiters.allocationFailed ? 5 : 100);
            SchedulerBlockInterruptible(proc);
            if (HasPendingSignals()) return -EINTR;
        }
    }

    if (fde->type == FdType::MemFd && fde->handle) {
        if ((FdStatus(fde) & 3) == 1) return -EBADF;
        FdPosition position(fde);
        return MemFdReadAt(static_cast<MemFdData*>(fde->handle),
            bufAddr, count, position.get(), true);
    }

    // Read from /dev/tty — reads from stdin pipe
    if (fde->type == FdType::DevTty && fde->handle)
    {
        auto* pair = static_cast<TtyDevicePair*>(fde->handle);
        auto* pipe = static_cast<PipeBuffer*>(pair->readPipe);
        // The ring copy runs under pipe->lock; read into kernel memory first.
        char staged[4096];

        for (;;)
        {
            uint32_t got = pipe->read(staged, static_cast<uint32_t>(
                count > 4096 ? 4096 : count));
            if (got > 0)
                return CopyToUser(bufAddr, staged, got) ? static_cast<int64_t>(got) : -EFAULT;

            uint32_t wr = __atomic_load_n(&pipe->writers, __ATOMIC_ACQUIRE);
            if (wr == 0) return 0;

            Process* self = ProcessCurrent();
            pipe->readerWaiter = self;
            self->wakeupTick = ApicTickCount() + 10;
            SchedulerBlock(self);
            if (HasReadInterruptSignal()) {
                if (pipe->readerWaiter == self) pipe->readerWaiter = nullptr;
                return -EINTR;
            }
        }
    }

    // A plain read consumes (and closes) ancillary rights accompanying bytes.
    if (fde->type == FdType::UnixSocket && fde->handle) {
        if(!count)return 0; // read(fd, ..., 0) does not consume a record.
        auto* socket=static_cast<UnixSocketData*>(fde->handle);
        UnixFdSnap descriptors[UNIX_FD_QUEUE_CAP];int received=0;
        auto result=UnixSocketReadSome(socket,(FdStatus(fde)&0x800)!=0,
            reinterpret_cast<char*>(bufAddr),count,descriptors,received);
        for(int i=0;i<received;++i)UnixFdSnapRelease(&descriptors[i]);
        return result;
    }

    return -EBADF;
}

// ---------------------------------------------------------------------------
// sys_readv (19)
// ---------------------------------------------------------------------------

static int64_t UnixRecordVectorIo(FdEntry* entry,uint64_t address,uint64_t count,bool writing)
{
    UnixRecordVector stage;if(int error=stage.Init(address,count,writing))return error;
    if(!stage.length)return 0; // readv/writev with an empty vector are no-ops.
    auto* socket=static_cast<UnixSocketData*>(entry->handle);
    if(writing)return UnixSocketWriteSome(socket,(FdStatus(entry)&0x800)!=0,stage.bytes,stage.length);
    UnixFdSnap descriptors[UNIX_FD_QUEUE_CAP];int received=0;
    auto result=UnixSocketReadSome(socket,(FdStatus(entry)&0x800)!=0,stage.bytes,stage.capacity,descriptors,received);
    for(int i=0;i<received;++i)UnixFdSnapRelease(&descriptors[i]);
    if(result>0)if(int error=stage.Scatter(result))return error;
    return result;
}

#include "unix_stream_vector.inc"

static int64_t PipeVectorIo(Process* proc,FdEntry* entry,uint64_t fd,
                            uint64_t address,uint64_t count,bool writing)
{
    struct Stage {
        iovec* vectors;char* bytes;
        ~Stage(){if(vectors)kfree(vectors);if(bytes)kfree(bytes);}
    } stage{static_cast<iovec*>(kmalloc(count?count*sizeof(iovec):1)),nullptr};
    if(!stage.vectors)return -ENOMEM;
    if(count && !CopyFromUser(stage.vectors,address,count*sizeof(iovec)))return -EFAULT;
    uint64_t length=0;
    for(uint64_t i=0;i<count;++i) {
        const auto& v=stage.vectors[i];
        if(v.iov_len>0x7fffffffULL-length)return -EINVAL;
        length+=v.iov_len;
        if(v.iov_len && !(writing?UserBufferReadable(v.iov_base,v.iov_len):UserBufferWritable(v.iov_base,v.iov_len)))return -EFAULT;
    }
    if(!length)return 0;
    uint64_t capacity=writing?4096:FdPipe(entry)->capacity;
    if(capacity>length)capacity=length;
    stage.bytes=static_cast<char*>(kmalloc(capacity));if(!stage.bytes)return -ENOMEM;
    uint64_t total=0,index=0,offset=0;
    if(!writing) {
        int64_t got=PipeReadSome(proc,entry,stage.bytes,capacity);
        if(got<=0)return got;
        for(uint64_t i=0;i<count && total<static_cast<uint64_t>(got);++i) {
            uint64_t n=stage.vectors[i].iov_len;
            if(n>static_cast<uint64_t>(got)-total)n=got-total;
            uint64_t copied=n?CopyToUserBytes(stage.vectors[i].iov_base,stage.bytes+total,n):0;
            total+=copied;
            if(copied<n)return total?static_cast<int64_t>(total):-EFAULT;
        }
        return got;
    }
    while(total<length) {
        uint64_t chunk=length-total;if(chunk>4096)chunk=4096;
        uint64_t copied=0;
        while(copied<chunk) {
            const auto& v=stage.vectors[index];
            uint64_t n=v.iov_len-offset;if(n>chunk-copied)n=chunk-copied;
            if(n && !CopyFromUser(stage.bytes+copied,v.iov_base+offset,n))return total?static_cast<int64_t>(total):-EFAULT;
            copied+=n;offset+=n;
            if(offset==v.iov_len){++index;offset=0;}
        }
        int64_t n=WriteToPinnedFdByType(proc,entry,fd,reinterpret_cast<uint64_t>(stage.bytes),chunk,false);
        if(n<0)return total?static_cast<int64_t>(total):n;
        total+=n;if(static_cast<uint64_t>(n)<chunk)break;
    }
    return total;
}

int64_t sys_readv(uint64_t fd, uint64_t iovAddr, uint64_t iovcnt,
                   uint64_t, uint64_t, uint64_t)
{
    if (iovcnt > 1024) return -EINVAL;
    // The generic loop rereads descriptors after nested sys_read scopes.
    // Keep their pages excluded until the entire outer operation finishes.
    UserAccessScope vectorAccess(ProcessCurrent());
    if (!UserBufferReadable(iovAddr, iovcnt * sizeof(iovec))) return -EFAULT;
    const auto* iov = reinterpret_cast<const iovec*>(iovAddr);
    Process* proc=ProcessCurrent();FdRef entry(proc,static_cast<int>(fd));
    if(!entry)return -EBADF;
    if(entry->type==FdType::NetlinkSocket)return NetlinkVectorIo(static_cast<NetlinkSocket*>(entry->handle),iovAddr,iovcnt,false);
    // One readv consumes one packet even across mode changes and iovecs.
    if(FdPipe(entry.get()))
        return PipeVectorIo(proc,entry.get(),fd,iovAddr,iovcnt,false);
    if(entry->type==FdType::UnixSocket && static_cast<UnixSocketData*>(entry->handle)->socketType==5)
        return UnixRecordVectorIo(entry.get(),iovAddr,iovcnt,false);
    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; ++i)
    {
        int64_t ret = sys_read(fd, iov[i].iov_base, iov[i].iov_len, 0, 0, 0);
        if (ret < 0) return (total > 0) ? total : ret;
        total += ret;
        if (static_cast<uint64_t>(ret) < iov[i].iov_len) break; // short read
    }
    return total;
}

// ---------------------------------------------------------------------------
// sys_writev (20)
// ---------------------------------------------------------------------------

int64_t sys_writev(uint64_t fd, uint64_t iovAddr, uint64_t iovcnt,
                    uint64_t, uint64_t, uint64_t)
{
    if (iovcnt > 1024) return -EINVAL;
    UserAccessScope vectorAccess(ProcessCurrent());
    if (!UserBufferReadable(iovAddr, iovcnt * sizeof(iovec))) return -EFAULT;
    const auto* iov = reinterpret_cast<const iovec*>(iovAddr);
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef ref(proc, static_cast<int>(fd));
    if (!ref) return -EBADF;
    auto* entry = ref.get();
    if(entry->type==FdType::NetlinkSocket)return NetlinkVectorIo(static_cast<NetlinkSocket*>(entry->handle),iovAddr,iovcnt,true);
    if(entry->type==FdType::UnixSocket && static_cast<UnixSocketData*>(entry->handle)->socketType==5)
        return UnixRecordVectorIo(entry,iovAddr,iovcnt,true);
    if(entry->type==FdType::UnixSocket && static_cast<UnixSocketData*>(entry->handle)->socketType==1)
        return UnixStreamWriteVector(entry,iovAddr,iovcnt);
    if(entry->type==FdType::Socket && entry->handle) {
        const int sockIdx=static_cast<int>(reinterpret_cast<uintptr_t>(entry->handle))-1;
        if(brook::SockIsStream(sockIdx)) {
            static_assert(sizeof(brook::TcpSendVector)==sizeof(iovec), "iovec ABI mismatch");
            auto* vectors=static_cast<brook::TcpSendVector*>(kmalloc(iovcnt ? iovcnt*sizeof(brook::TcpSendVector) : 1));
            if(!vectors)return -ENOMEM;
            if(iovcnt && !CopyFromUser(vectors,iovAddr,iovcnt*sizeof(*vectors))) { kfree(vectors);return -EFAULT; }
            uint64_t total=0;
            for(uint64_t i=0;i<iovcnt;++i) {
                if(vectors[i].length>0x7fffffffffffffffULL-total) { kfree(vectors);return -EINVAL; }
                total+=vectors[i].length;
            }
            const uint32_t bytes=static_cast<uint32_t>(total>0x7ffff000ULL ? 0x7ffff000ULL : total);
            const int result=bytes ? brook::SockSendUserVector(sockIdx,vectors,static_cast<unsigned>(iovcnt),bytes) : 0;
            kfree(vectors);
            return result;
        }
    }
    if(FdPipe(entry) && (FdStatus(entry)&0x4000))
        return PipeVectorIo(proc,entry,fd,iovAddr,iovcnt,true);
    if (entry->type == FdType::Vnode && entry->handle && (FdStatus(entry) & 0x400)) {
        if ((FdStatus(entry) & 3) == 0) return -EBADF;
        // Snapshot descriptors before validation: users may change the array.
        auto* vectors = static_cast<iovec*>(kmalloc(iovcnt ? iovcnt * sizeof(iovec) : 1));
        if (!vectors) return -ENOMEM;
        memcpy(vectors, iov, iovcnt * sizeof(iovec));
        uint64_t bytes = 0;
        for (uint64_t i = 0; i < iovcnt; ++i) {
            if (vectors[i].iov_len > 0x7fffffffffffffffULL - bytes) {
                kfree(vectors);return -EINVAL;
            }
            bytes += vectors[i].iov_len;
        }
        uint64_t capacity = bytes < AppendStageLimit ? bytes : AppendStageLimit;
        auto* staged = static_cast<uint8_t*>(kmalloc(capacity ? capacity : 1));
        if (!staged) { kfree(vectors);return -ENOMEM; }
        uint64_t copied = 0;
        for (uint64_t i = 0; i < iovcnt && copied < capacity; ++i) {
            uint64_t length = vectors[i].iov_len;
            if (length > capacity - copied) length = capacity - copied;
            if (length && !UserBufferReadable(vectors[i].iov_base, length)) {
                kfree(staged);kfree(vectors);return -EFAULT;
            }
            if (length) memcpy(staged + copied, reinterpret_cast<const void*>(vectors[i].iov_base), length);
            copied += length;
        }
        kfree(vectors);
        auto* vn = static_cast<Vnode*>(entry->handle);
        int64_t result;
        {
            FdPosition position(entry);
            result = VfsAppend(vn, staged, capacity, position.get());
        }
        kfree(staged);
        if (result > 0 && (FdStatus(entry) & 0x1000)) {
            int error = VfsFsync(vn);if (error) return error;
        }
        return result;
    }
    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; ++i)
    {
        int64_t ret = sys_write(fd, iov[i].iov_base, iov[i].iov_len, 0, 0, 0);
        if (ret < 0) return (total > 0) ? total : ret;
        total += ret;
        if (static_cast<uint64_t>(ret) < iov[i].iov_len) break;
    }
    return total;
}

// ---------------------------------------------------------------------------
// sys_sendfile (40) — stub: copies between fds in kernel
// ---------------------------------------------------------------------------

int64_t sys_sendfile(uint64_t out_fd, uint64_t in_fd, uint64_t offsetAddr,
                      uint64_t count, uint64_t, uint64_t)
{
    out_fd=static_cast<uint32_t>(out_fd);in_fd=static_cast<uint32_t>(in_fd);
    Process* proc=ProcessCurrent();if(!proc)return -EBADF;
    KernelOperationGuard operation(proc);
    FdRef input(proc,static_cast<int>(in_fd)),output(proc,static_cast<int>(out_fd));
    if(!input || !output)return -EBADF;
    auto* out_fde=output.get();
    if(out_fde->type==FdType::Path)return -EBADF;
    if(FdStatus(out_fde)&0x400u)return -EINVAL; // Linux forbids append output.
    if((out_fde->type==FdType::MemFd || (out_fde->type==FdType::Vnode && out_fde->handle)) &&
       (FdStatus(out_fde)&3)==0)return -EBADF;
    auto* in_fde=input.get();
    if(in_fde->type==FdType::Path || (FdStatus(in_fde)&3)==1)return -EBADF;
    if((in_fde->type!=FdType::Vnode && in_fde->type!=FdType::MemFd) || !in_fde->handle)return -EINVAL;
    // Like Linux, sendfile reads and stores the shared offset without
    // f_pos_lock (the output may be the same description).
    uint64_t offset=OpenFileOffset(in_fde->file);
    if(offsetAddr) {
        if(!UserBufferWritable(offsetAddr,8) || !CopyFromUser(&offset,offsetAddr,8))return -EFAULT;
    }
    if(offset>0x7fffffffffffffffULL)return -EINVAL;
    if(!count)return 0;
    if(count>256*1024)count=256*1024;
    uint32_t chunk=count>4096?4096:static_cast<uint32_t>(count);
    auto* bounce=static_cast<uint8_t*>(kmalloc(chunk));if(!bounce)return -ENOMEM;
    int64_t total=0,error=0;
    while(static_cast<uint64_t>(total)<count) {
        uint64_t remaining=count-static_cast<uint64_t>(total),amount=remaining<chunk?remaining:chunk;
        uint64_t readOffset=offset;
        int64_t read=in_fde->type==FdType::MemFd?
            MemFdReadAt(static_cast<MemFdData*>(in_fde->handle),reinterpret_cast<uint64_t>(bounce),amount,&readOffset,false):
            VfsRead(static_cast<Vnode*>(in_fde->handle),bounce,amount,&readOffset);
        if(read<=0) {error=read;break;}
        int64_t written=WriteToPinnedFdByType(proc,output.get(),out_fd,reinterpret_cast<uint64_t>(bounce),static_cast<uint64_t>(read),false);
        if(written<=0 || written>read) {error=written<0?written:-EIO;break;}
        offset+=static_cast<uint64_t>(written);total+=written;
        if(written<read)break;
    }
    kfree(bounce);
    if(offsetAddr) {if(!CopyToUser(offsetAddr,&offset,8))return -EFAULT;}
    else OpenFileSetOffset(in_fde->file,offset);
    return total?total:error;
}

// ---------------------------------------------------------------------------
// sys_pwrite64 (18) — write at offset without changing file position
// ---------------------------------------------------------------------------

int64_t sys_pwrite64(uint64_t fd, uint64_t bufAddr, uint64_t count,
                      uint64_t offset, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef ref(proc, static_cast<int>(fd));
    FdEntry* fde = ref.get();
    if(FdPipe(fde))return -29;
    if (!fde || fde->type==FdType::Path) return -EBADF;
    if (offset > 0x7fffffffffffffffULL) return -EINVAL;
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)
    if(fde->type==FdType::BlockDevice && fde->handle) {
        if((FdStatus(fde)&3)==0)return -EBADF;
        if(count && !UserBufferReadable(bufAddr,count))return -EFAULT;
        auto* block=static_cast<MaintenanceBlockFd*>(fde->handle);
        int64_t result=MaintenanceBlockTransfer(block,bufAddr,count,&offset,true,true);
        if(result>0 && (FdStatus(fde)&0x1000)) {const int error=DeviceFlush(block->device);if(error)result=error;}
        return result;
    }
#endif
    if (fde->type == FdType::MemFd && fde->handle) {
        if ((FdStatus(fde) & 3) == 0) return -EBADF;
        if (count && !UserBufferReadable(bufAddr, count)) return -EFAULT;
        return MemFdWriteAt(static_cast<MemFdData*>(fde->handle),
            bufAddr, count, &offset,
            (FdStatus(fde) & 0x400) != 0, true);
    }
    if (fde->type != FdType::Vnode || !fde->handle) return -EINVAL;
    if ((FdStatus(fde) & 3) == 0) return -EBADF;
    if (count && !UserBufferReadable(bufAddr, count)) return -EFAULT;

    auto* vn = static_cast<Vnode*>(fde->handle);
    const void* buf = reinterpret_cast<const void*>(bufAddr);
    uint64_t off = offset;
    // Linux pwrite on O_APPEND chooses EOF but leaves the FD offset unchanged.
    int64_t result = (FdStatus(fde) & 0x400)
        ? AppendUserBuffer(vn, bufAddr, count, &off)
        : VfsWrite(vn, buf, count < VnodeWriteLimit ? count : VnodeWriteLimit, &off);
    if (result > 0 && (FdStatus(fde) & 0x1000)) {
        int error = VfsFsync(vn);
        if (error) return error;
    }
    return result;
}

// ---------------------------------------------------------------------------
// sys_pread64 (17) — read at offset without changing file position
// ---------------------------------------------------------------------------

int64_t sys_pread64(uint64_t fd, uint64_t bufAddr, uint64_t count,
                     uint64_t offset, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef ref(proc, static_cast<int>(fd));
    FdEntry* fde = ref.get();
    if(FdPipe(fde))return -29;
    if (!fde || fde->type==FdType::Path) return -EBADF;

    if (offset > 0x7fffffffffffffffULL) return -EINVAL;
    if (count && !UserBufferWritable(bufAddr, count)) return -EFAULT;
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS) || defined(BROOK_SSD_INSTALLER)
    if(fde->type==FdType::BlockDevice && fde->handle) {
        if((FdStatus(fde)&3)==1)return -EBADF;
        return MaintenanceBlockTransfer(static_cast<MaintenanceBlockFd*>(fde->handle),bufAddr,count,&offset,false,true);
    }
#endif
    if (fde->type == FdType::MemFd && fde->handle) {
        if ((FdStatus(fde) & 3) == 1) return -EBADF;
        return MemFdReadAt(static_cast<MemFdData*>(fde->handle),
            bufAddr, count, &offset, true);
    }
    if (fde->type == FdType::Vnode && fde->handle)
    {
        if (!FdOpenForReading(fde)) return -EBADF;
        auto* vn = static_cast<Vnode*>(fde->handle);
        uint64_t pos = offset;
        return VfsRead(vn, reinterpret_cast<void*>(bufAddr),
                       count, &pos);
    }

    return -EBADF;
}

} // namespace brook
