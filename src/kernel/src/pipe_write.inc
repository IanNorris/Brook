// Actual sys_write pipe branch, shared with hosted syscall tests.
    // Write to pipe — block until at least some bytes are written
    if (fde->type == FdType::Pipe && fde->handle)
    {
        // For bidirectional socketpair (flags==2), write goes to the write pipe
        auto* pipe = (fde->flags == 2)
            ? reinterpret_cast<PipeBuffer*>(fde->seekPos)
            : static_cast<PipeBuffer*>(fde->handle);
        const char* src = reinterpret_cast<const char*>(bufAddr);
        uint64_t written = 0;

        while (written < count)
        {
            // If no readers, send SIGPIPE and return EPIPE
            if (__atomic_load_n(&pipe->readers, __ATOMIC_ACQUIRE) == 0)
            {
                constexpr int SIGPIPE = 13;
                Process* self = ProcessCurrent();
                if (self)
                    ProcessSendSignal(self, SIGPIPE);
                return written > 0 ? static_cast<int64_t>(written) : -EPIPE;
            }

            uint32_t chunk = pipe->write(src + written,
                static_cast<uint32_t>(count - written > 4096 ? 4096 : count - written));
            written += chunk;
            if (written > 0)
            {
                // Wake blocked reader
                Process* reader = pipe->readerWaiter;
                if (reader)
                {
                    pipe->readerWaiter = nullptr;
                    WakeProcess(reader);
                }
                PipeWakeEpoll(pipe);
                break;  // Return partial writes immediately
            }
            // A control/self-pipe may be drained only by this same caller.
            // Blocking here despite O_NONBLOCK can deadlock its event loop.
            SpinLockAcquire(ProcessFdLock(proc));
            const bool nonblock = (fde->statusFlags & 0x800) != 0;
            SpinLockRelease(ProcessFdLock(proc));
            if (nonblock) return -EAGAIN;
            // Buffer full — block until reader drains some
            Process* self = ProcessCurrent();
            pipe->writerWaiter = self;
            // Re-check after registering waiter to close the race where
            // a reader drained space between our write() and waiter set.
            chunk = pipe->write(src + written,
                static_cast<uint32_t>(count - written > 4096 ? 4096 : count - written));
            if (chunk > 0)
            {
                pipe->writerWaiter = nullptr;
                written += chunk;
                Process* reader = pipe->readerWaiter;
                if (reader)
                {
                    pipe->readerWaiter = nullptr;
                    WakeProcess(reader);
                }
                PipeWakeEpoll(pipe);
                break;
            }
            __atomic_fetch_add(&pipe->fullWriteWaits,1,__ATOMIC_RELAXED);
            // Safety timeout — recheck every 10ms in case wakeup was missed
            self->wakeupTick = ApicTickCount() + 10;
            SchedulerBlock(self);
            if (HasPendingSignals())
                return written > 0 ? static_cast<int64_t>(written) : -EINTR;
        }
        return static_cast<int64_t>(written);
    }
