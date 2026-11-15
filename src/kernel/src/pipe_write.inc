// Actual sys_write pipe branch, shared with hosted syscall tests.
    // Write to pipe — block until at least some bytes are written
    if (fde->type == FdType::Pipe && fde->handle)
    {
        if(fde->flags==0)return -EBADF;
        auto* pipe = static_cast<PipeBuffer*>(fde->handle);
        KernelOperationGuard operation(proc);
        // PipeBuffer::write copies under pipe->lock, so a user source is
        // staged one chunk at a time first (a fault returns EFAULT, or the
        // bytes already written). Kernel sources are used in place.
        const char* src = reinterpret_cast<const char*>(bufAddr);
        char stage[4096];
        uint64_t written = 0;
        // O_DIRECT is a shared status flag of the description (H-20).
        const bool packet = (FdStatus(fde) & 0x4000) != 0;
        if (!count) return 0;
        if (packet && !pipe->enablePackets()) return -ENOMEM;

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

            const uint32_t want = static_cast<uint32_t>(count - written > 4096 ? 4096 : count - written);
            const char* chunkSource = src + written;
            if (userSource) {
                if (!CopyFromUser(stage, bufAddr + written, want))
                    return written > 0 ? static_cast<int64_t>(written) : -EFAULT;
                chunkSource = stage;
            }
            uint32_t chunk = pipe->write(chunkSource, want,
                (fde->flags&0x10) && count<=4096, packet);
            written += chunk;
            if (chunk > 0)
            {
                // Wake blocked reader
                SpinLockAcquire(&pipe->lock);
                Process* reader=pipe->readerWaiter;pipe->readerWaiter=nullptr;
                if(reader)WakeProcess(reader);
                SpinLockRelease(&pipe->lock);
                PipeWakeEpoll(pipe);
                if(packet && written<count)continue;
                break;  // Stream pipes retain their existing partial-write policy.
            }
            // A control/self-pipe may be drained only by this same caller.
            // Blocking here despite O_NONBLOCK can deadlock its event loop.
            SpinLockAcquire(ProcessFdLock(proc));
            const bool nonblock = (FdStatus(fde) & 0x800) != 0;
            SpinLockRelease(ProcessFdLock(proc));
            if (nonblock) return written ? static_cast<int64_t>(written) : -EAGAIN;
            // Buffer full — block until reader drains some
            Process* self = ProcessCurrent();
            PipePollGroup writerWaiters;
            if(!writerWaiters.add(pipe,self,4|8))return written ? static_cast<int64_t>(written) : -ENOMEM;
            // Re-check after registering waiter to close the race where
            // a reader drained space between our write() and waiter set.
            chunk = pipe->write(chunkSource, want,
                (fde->flags&0x10) && count<=4096, packet);
            if (chunk > 0)
            {
                written += chunk;
                SpinLockAcquire(&pipe->lock);
                Process* reader=pipe->readerWaiter;pipe->readerWaiter=nullptr;
                if(reader)WakeProcess(reader);
                SpinLockRelease(&pipe->lock);
                PipeWakeEpoll(pipe);
                if(packet && written<count)continue;
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
