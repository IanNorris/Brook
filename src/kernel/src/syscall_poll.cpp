// syscall_poll.cpp -- poll/ppoll/select/pselect6 and shared wait-deadline helpers.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "signal_fd.h"
#include "sync_file.h"
#include "net.h"
#include "syscall_internal.h"
#include "syscall_signal.h"
#include "syscall_eventfd.h"
#include "syscall_timerfd.h"
#include "syscall_dsp.h"
#include "syscall_pipe.h"
#include "syscall_unix_socket.h"
#include "syscall_time.h"
#include "syscall_poll.h"
#include "syscall_epoll.h"
#include "syscall_netlink.h"
#include "syscall_handlers.h"

namespace brook {

// ---------------------------------------------------------------------------
// sys_poll (7) — basic poll implementation
// ---------------------------------------------------------------------------

static constexpr short POLLPRI = 0x0002;
static constexpr short POLLOUT = 0x0004;
static constexpr short POLLERR = 0x0008;
static constexpr short POLLHUP = 0x0010;
static constexpr short POLLNVAL = 0x0020;

// LAPIC ticks schedule wakeups, but can drift relative to CLOCK_MONOTONIC.
// As with nanosleep, only the monotonic deadline can expire a user timeout.
uint64_t WaitDeadlineNs(uint64_t timeoutMs)
{
    uint64_t now = ClockNowNs(1);
    if (timeoutMs == ~0ULL || timeoutMs > (~0ULL - now) / 1000000ULL)
        return ~0ULL;
    return now + timeoutMs * 1000000ULL;
}

uint64_t WaitWakeupTick(uint64_t deadlineNs, uint64_t safetyMs)
{
    uint64_t now = ClockNowNs(1);
    uint64_t remaining = deadlineNs > now ? deadlineNs - now : 0;
    uint64_t ticks = remaining / 1000000ULL + (remaining % 1000000ULL != 0);
    if (ticks > safetyMs) ticks = safetyMs;
    return ApicTickCount() + ticks;
}

// Trusted kernel array; syscall wrappers validate/copy user memory separately.
int EpollPollReady(Process* proc, EpollInstance* ep);

int64_t PollWait(pollfd* fds, uint64_t nfds, uint64_t timeout_ms, bool selectMode)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EFAULT;
    const bool infinite = timeout_ms == ~0ULL;
    const uint64_t deadline = WaitDeadlineNs(timeout_ms);

    // Poll diagnostics are opt-in, like the syscall trace itself. Printing
    // each RequestServer poll directly to the UART stalls browser IPC and
    // holds interrupts off, even when ordinary strace is disabled.
    bool logPoll = false;
    // SC_IO is declared with the strace dispatcher below.
    constexpr uint32_t pollTraceCategory = 1u << 5;
    logPoll = proc->straceEnabled && !proc->straceErrorsOnly &&
        (proc->straceFilter == 0 || (proc->straceFilter & pollTraceCategory));
    if (logPoll) {
        for (uint64_t i = 0; i < nfds && i < 8; i++) {
            FdEntry* fde = fds[i].fd >= 0 ? FdGet(proc, fds[i].fd) : nullptr;
            const char* tname = "?";
            if (fde) {
                switch (fde->type) {
                    case FdType::Socket: tname = "inet"; break;
                    case FdType::UnixSocket: tname = "unix"; break;
                    case FdType::Pipe: tname = "pipe"; break;
                    case FdType::EventFd: tname = "evfd"; break;
                    case FdType::SyncFile: tname = "sync_file"; break;
                    case FdType::TimerFd: tname = "tmfd"; break;
                    default: tname = "oth"; break;
                }
            }
            SerialPrintf("%s%d(%s,ev=0x%x)", i ? "," : "",
                         fds[i].fd, tname, fds[i].events);
        }
        if (nfds > 8) SerialPrintf(",... +%lu more", nfds - 8);
        SerialPrintf("]\n");
    }

retry_poll:
    int ready = 0;
    for (uint64_t i = 0; i < nfds; i++)
    {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;

        FdRef fde_ref(proc, fds[i].fd);  // BRO-156: pin across handle deref
        FdEntry* fde = fde_ref.get();

        if (!fde || fde->type == FdType::None)
        {
            fds[i].revents = POLLNVAL;
            ready++;
            continue;
        }

        if(FdNamedFifo(fde)) {
            fds[i].revents=static_cast<int16_t>(NamedFifoReady(fde,fds[i].events));
            if(fds[i].revents)++ready;
            continue;
        }
        // Pipes: check readability/writability
        if (fde->type == FdType::Pipe && fde->handle)
        {
            auto* pb = static_cast<PipeBuffer*>(fde->handle);
            bool isWrite = (fde->flags & 1);
            if (!isWrite && (fds[i].events & POLLIN))
            {
                if (pb->count() > 0 || pb->writers == 0)
                {
                    fds[i].revents |= (pb->count() > 0) ? POLLIN : POLLHUP;
                    ready++;
                }
            }
            if (isWrite && (fds[i].events & POLLOUT))
            {
                if (pb->space() > 0 || pb->readers == 0)
                {
                    fds[i].revents |= (pb->readers > 0) ? POLLOUT : POLLERR;
                    ready++;
                }
            }
            continue;
        }

        // An epoll fd is readable when a watched event is available, and is
        // never writable. Do not fall through to synthetic device readiness.
        if (fde->type == FdType::EpollFd && fde->handle) {
            if (fds[i].events & POLLIN) {
                int result = EpollPollReady(proc, static_cast<EpollInstance*>(fde->handle));
                if (result < 0) return result;
                if (result) { fds[i].revents = POLLIN; ready++; }
            }
            continue;
        }

        // Regular files are always ready
        if(fde->type==FdType::Path) {fds[i].revents=POLLNVAL;++ready;continue;}
        if (fde->type == FdType::Vnode)
        {
            if (fds[i].events & POLLIN)  fds[i].revents |= POLLIN;
            if (fds[i].events & POLLOUT) fds[i].revents |= POLLOUT;
            if (fds[i].revents) ready++;
            continue;
        }

        // Keyboard device: check if input available
        if (fde->type == FdType::DevKeyboard)
        {
            if (fds[i].events & POLLIN)
            {
                if (InputHasEvents())
                {
                    fds[i].revents |= POLLIN;
                    ready++;
                }
            }
            continue;
        }

        if(fde->type==FdType::NetlinkSocket) {
            fds[i].revents=static_cast<int16_t>(NetlinkReady(static_cast<NetlinkSocket*>(fde->handle),fds[i].events));
            if(fds[i].revents)++ready;continue;
        }
        // Socket: check TCP/UDP readiness
        if (fde->type == FdType::Socket && fde->handle)
        {
            int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
            bool rdReady = (fds[i].events & POLLIN) && brook::SockPollReady(sockIdx, true, false);
            bool wrReady = (fds[i].events & POLLOUT) && brook::SockPollReady(sockIdx, false, true);
            bool hupReady = brook::SockPollHangup(sockIdx);

            if (rdReady) fds[i].revents |= POLLIN;
            if (wrReady) fds[i].revents |= POLLOUT;
            if (hupReady) fds[i].revents |= POLLHUP;
            if (fds[i].revents) ready++;
            continue;
        }

        // Unix domain socket
        if (fde->type == FdType::UnixSocket && fde->handle)
        {
            auto* usd = static_cast<UnixSocketData*>(fde->handle);
            fds[i].revents=static_cast<short>(UnixSocketReady(usd,static_cast<uint16_t>(fds[i].events)));
            if (fds[i].revents) ready++;
            continue;
        }

        // /dev/tty: check read pipe for POLLIN, write pipe always ready for POLLOUT
        if (fde->type == FdType::DevTty && fde->handle)
        {
            auto* pair = static_cast<TtyDevicePair*>(fde->handle);
            if (fds[i].events & POLLIN)
            {
                auto* rp = static_cast<PipeBuffer*>(pair->readPipe);
                if (rp->count() > 0 || rp->writers == 0)
                {
                    fds[i].revents |= POLLIN;
                }
            }
            if (fds[i].events & POLLOUT)
                fds[i].revents |= POLLOUT;
            if (fds[i].revents) ready++;
            continue;
        }

        if(fde->type==FdType::SyncFile && fde->handle) {
            fds[i].revents|=SyncFileReady(*static_cast<SyncFileData*>(fde->handle),fds[i].events);
            if(fds[i].revents)++ready;
            continue;
        }

        if(fde->type==FdType::SignalFd && fde->handle) {
            fds[i].revents=SignalFdReady(proc,static_cast<SignalFd*>(fde->handle),fds[i].events);
            if(fds[i].revents)++ready;
            continue;
        }

        // EventFd: readable when nonempty, writable below counter capacity.
        if (fde->type == FdType::EventFd && fde->handle)
        {
            auto* efd = static_cast<EventFdData*>(fde->handle);
            fds[i].revents |= EventFdReady(efd, fds[i].events).ready;
            if (fds[i].revents) ready++;
            continue;
        }

        if (fde->type == FdType::TimerFd && fde->handle)
        {
            auto* tfd = static_cast<TimerFdData*>(fde->handle);
            if (fds[i].events & POLLIN)
            {
                if (TimerFdReady(tfd)) fds[i].revents |= POLLIN;
            }
            if (fds[i].revents) ready++;
            continue;
        }

        if (fde->type == FdType::MemFd)
        {
            if (fds[i].events & (POLLIN | POLLOUT))
                fds[i].revents |= (fds[i].events & (POLLIN | POLLOUT));
            if (fds[i].revents) ready++;
            continue;
        }

        // Cached per-stream capacity: descriptor scans must not call the sleeping driver.
        if (fde->type == FdType::DevDsp)
        {
            AudioStreamProgress progress{};
            auto* dsp = static_cast<DspState*>(fde->handle);
            if (dsp && AudioMixerGetProgress(dsp->mixerStreamId, &progress)) {
                if ((fds[i].events & POLLOUT) &&
                    (progress.discontinuity || AudioStreamWritable(progress)))
                    fds[i].revents |= POLLOUT;
            } else fds[i].revents |= POLLERR;
            if (fds[i].revents) ready++;
            continue;
        }

        // Ordinary synthetic descriptors are readable/writable; exceptional
        // conditions must never be invented for select's exceptfds.
        fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
        if (fds[i].revents) ready++;
    }

    // select maps hangup/error into requested read/write sets, never exceptfds.
    if (selectMode) {
        ready = 0;
        for (uint64_t i = 0; i < nfds; ++i) {
            short allowed = POLLNVAL;
            if (fds[i].events & POLLIN) allowed |= POLLIN | POLLHUP | POLLERR;
            if (fds[i].events & POLLOUT) allowed |= POLLOUT | POLLERR;
            if (fds[i].events & POLLPRI) allowed |= POLLPRI;
            fds[i].revents &= allowed;
            if (fds[i].revents) ++ready;
        }
    }

    // If nothing ready and timeout != 0, block and retry
    if (ready == 0 && timeout_ms != 0 && (infinite || ClockNowNs(1) < deadline))
    {
        // Register as waiter BEFORE re-checking data availability.
        // This closes the race where data arrives between the initial
        // scan and the block call.
        Process* self = ProcessCurrent();

        // For pipe FDs, register as waiter on the pipe
        uint64_t waiterCap = nfds;
        uint64_t safetyMs = 50;
        uint64_t tcpPoolWaitToken = 0;
        NetlinkWaitGroup netlinkWaiters;
        TimerWaitGroup timerWaiters;
        PipePollGroup pipeWaiters;
        EventFdWaitGroup eventWaiters;
        SyncFileWait syncWait;
        // Allocate pipe subscriptions before installing legacy wait hooks, so
        // ENOMEM unwinds only RAII-owned exact registrations, never stale hooks.
        for (uint64_t i = 0; i < waiterCap; ++i) {
            if (fds[i].fd < 0) continue;
            FdRef fde_ref(proc, fds[i].fd);
            FdEntry* fde = fde_ref.get();
            if (!fde) continue;
            if(fde->type==FdType::NetlinkSocket && !netlinkWaiters.add(static_cast<NetlinkSocket*>(fde->handle),self))return -ENOMEM;
            if(FdNamedFifo(fde)) {
                if(!pipeWaiters.add(FdPipe(fde),self,fds[i].events|POLLHUP|POLLERR))return -ENOMEM;
            }
            if (fde->type == FdType::Pipe && fde->handle && (fds[i].events & POLLIN))
            {
                auto* pb = static_cast<PipeBuffer*>(fde->handle);
                if (!(fde->flags & 1)) // read end
                    if (!pipeWaiters.add(pb, self, POLLIN)) return -ENOMEM;
            }
            // Write end: a reader draining (or closing) notifies POLLOUT.
            if (fde->type == FdType::Pipe && fde->handle && (fds[i].events & POLLOUT) && fde->flags == 1)
            {
                if (!pipeWaiters.add(static_cast<PipeBuffer*>(fde->handle), self, POLLOUT)) return -ENOMEM;
            }
            if (fde->type == FdType::DevTty && fde->handle && (fds[i].events & POLLIN))
            {
                auto* pair = static_cast<TtyDevicePair*>(fde->handle);
                auto* rp = static_cast<PipeBuffer*>(pair->readPipe);
                if (!pipeWaiters.add(rp, self, POLLIN)) return -ENOMEM;
            }
            if (fde->type == FdType::UnixSocket && fde->handle)
            {
                auto* usd = static_cast<UnixSocketData*>(fde->handle);
                if ((fds[i].events & POLLIN) &&
                    usd->state == UnixSocketData::State::Connected && usd->rxPipe)
                {
                    if (!pipeWaiters.add(usd->rxPipe, self, POLLIN)) return -ENOMEM;
                }
                if ((fds[i].events & POLLOUT) &&
                    usd->state == UnixSocketData::State::Connected && usd->txPipe)
                {
                    if (!pipeWaiters.add(usd->txPipe, self, POLLOUT)) return -ENOMEM;
                }
            }
        }
        for (uint64_t i = 0; i < waiterCap; i++)
        {
            if (fds[i].fd < 0) continue;
            FdRef fde_ref(proc, fds[i].fd);  // BRO-156: pin across handle deref
            FdEntry* fde = fde_ref.get();
            if (!fde) continue;
            if (fde->type == FdType::TimerFd && fde->handle && (fds[i].events & POLLIN))
                timerWaiters.add(static_cast<TimerFdData*>(fde->handle), self);
            if (fde->type == FdType::DevDsp) safetyMs = 5; // Cached DMA readiness has no IRQ waiter yet.
            if (fde->type == FdType::DevKeyboard && (fds[i].events & POLLIN))
                InputAddWaiter(self);
            if (fde->type == FdType::Socket && fde->handle)
            {
                int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
                brook::SockSetPollWaiter(sockIdx, self);
                if((fds[i].events & POLLOUT) && !tcpPoolWaitToken)
                    tcpPoolWaitToken=brook::NetTcpPoolWaitRegister(self);
            }
            if (fde->type == FdType::UnixSocket && fde->handle) {
                auto* usd = static_cast<UnixSocketData*>(fde->handle);
                if ((fds[i].events & POLLIN) && usd->state == UnixSocketData::State::Listening)
                    usd->epollWaiter = self;
            }
            if (fde->type == FdType::EventFd && fde->handle &&
                !eventWaiters.add(static_cast<EventFdData*>(fde->handle), self))
                safetyMs = 5;
            if (fde->type == FdType::SyncFile && fde->handle && (fds[i].events & POLLIN))
                syncWait.Want(*static_cast<SyncFileData*>(fde->handle));
            // No wake source yet: signals aimed at another thread of the
            // process, and nested epoll readiness. Poll these more often.
            if ((fde->type == FdType::SignalFd || fde->type == FdType::EpollFd) && safetyMs > 10)
                safetyMs = 10;
        }
        // A fence that retired since the snapshot means: rescan, don't sleep.
        if (syncWait.Arm()) ready++;

        // Re-check data availability after registration.
        // If data arrived between the initial scan and registration,
        // we catch it here instead of blocking forever.
        for (uint64_t i = 0; i < nfds; i++)
        {
            if (fds[i].fd < 0) continue;
            FdRef fde_ref(proc, fds[i].fd);  // BRO-156: pin across handle deref
            FdEntry* fde = fde_ref.get();
            if (!fde) continue;
            if (fde->type == FdType::TimerFd && fde->handle && (fds[i].events & POLLIN) &&
                TimerFdReady(static_cast<TimerFdData*>(fde->handle)))
            { ready++; break; }
            if (fde->type == FdType::DevKeyboard && (fds[i].events & POLLIN) && InputHasEvents())
            { ready++; break; }
            if(fde->type==FdType::NetlinkSocket && NetlinkReady(static_cast<NetlinkSocket*>(fde->handle),fds[i].events)){++ready;break;}
            if(FdNamedFifo(fde) && NamedFifoReady(fde,fds[i].events)) {ready++;break;}
            if (fde->type == FdType::Pipe && fde->handle && (fds[i].events & POLLIN))
            {
                auto* pb = static_cast<PipeBuffer*>(fde->handle);
                if (!(fde->flags & 1) && (pb->count() > 0 || pb->writers == 0))
                { ready++; break; }
            }
            if (fde->type == FdType::Pipe && fde->handle && (fds[i].events & POLLOUT) && fde->flags == 1)
            {
                auto* pb = static_cast<PipeBuffer*>(fde->handle);
                if (pb->space() > 0 || __atomic_load_n(&pb->readers, __ATOMIC_ACQUIRE) == 0)
                { ready++; break; }
            }
            if (fde->type == FdType::SyncFile && fde->handle && (fds[i].events & POLLIN) &&
                SyncFileReady(*static_cast<SyncFileData*>(fde->handle), POLLIN))
            { ready++; break; }
            if (fde->type == FdType::Socket && fde->handle)
            {
                int sockIdx = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
                if (brook::SockPollReady(sockIdx, (fds[i].events & POLLIN) != 0,
                                                  (fds[i].events & POLLOUT) != 0) ||
                    ((!selectMode || (fds[i].events & (POLLIN | POLLOUT))) &&
                     brook::SockPollHangup(sockIdx)))
                { ready++; break; }
            }
            if (fde->type == FdType::UnixSocket && fde->handle)
            {
                auto* usd = static_cast<UnixSocketData*>(fde->handle);
                if ((fds[i].events & POLLIN) && usd->state == UnixSocketData::State::Listening) {
                    if (usd->pendingCount > 0) { ready++; break; }
                } else if ((fds[i].events & POLLIN) &&
                           usd->state == UnixSocketData::State::Connected && usd->rxPipe) {
                    if (usd->rxPipe->count() > 0 ||
                        __atomic_load_n(&usd->rxPipe->writers, __ATOMIC_ACQUIRE) == 0)
                    { ready++; break; }
                }
                if ((fds[i].events & POLLOUT) &&
                    usd->state == UnixSocketData::State::Connected && usd->txPipe &&
                    (usd->txPipe->space() > 0 ||
                     __atomic_load_n(&usd->txPipe->readers, __ATOMIC_ACQUIRE) == 0))
                { ready++; break; }
            }
            if (fde->type == FdType::EventFd && fde->handle && (fds[i].events & POLLIN))
            {
                auto* efd = static_cast<EventFdData*>(fde->handle);
                if (EventFdReady(efd, fds[i].events).ready)
                { ready++; break; }
            }
        }

        if (ready == 0)
        {
            // Keep one absolute deadline across spurious wakes. Periodic
            // checks also service timerfds and descriptors without wait hooks.
            const uint64_t timerDeadline = timerWaiters.deadline(deadline);
            if (timerWaiters.allocationFailed) safetyMs = 5;
            safetyMs = syncWait.SafetyMs(safetyMs);
            self->wakeupTick = WaitWakeupTick(timerDeadline, safetyMs);
            if (!HasPendingSignals() && (infinite || ClockNowNs(1) < deadline))
                SchedulerBlockInterruptible(self);

        }

        // Clean up waiters
        brook::NetTcpPoolWaitUnregister(tcpPoolWaitToken);
        InputRemoveWaiter(self);
        for (uint64_t i = 0; i < waiterCap; i++)
        {
            if (fds[i].fd < 0) continue;
            FdRef fde_ref(proc, fds[i].fd);  // BRO-156: pin across handle deref
            FdEntry* fde = fde_ref.get();
            if (!fde) continue;
            if (fde->type == FdType::UnixSocket && fde->handle)
            {
                auto* usd = static_cast<UnixSocketData*>(fde->handle);
                if ((fds[i].events & POLLIN) &&
                    usd->state == UnixSocketData::State::Listening &&
                    usd->epollWaiter == self)
                    usd->epollWaiter = nullptr;
            }
            if (fde->type == FdType::Socket && fde->handle)
            {
                int si = static_cast<int>(reinterpret_cast<uintptr_t>(fde->handle)) - 1;
                brook::SockClearPollWaiter(si, self);
            }
        }

        // Both ordinary signal interruption and terminal cancellation use the
        // complete common unregister path (including DevTty) before unwinding.
        if (HasPendingSignals()) return -EINTR;

        // Always use the same readiness scan before/after sleeping, including
        // timerfds, sockets and eventfds. Expiry is checked after that scan.
        goto retry_poll;
    }

    if (logPoll && ready > 0) {
        int logged = 0;
        for (uint64_t i = 0; i < nfds && logged < 8; i++) {
            if (fds[i].revents) {
                SerialPrintf("%s%d(0x%x)", logged ? "," : "",
                             fds[i].fd, fds[i].revents);
                logged++;
            }
        }
        SerialPrintf("]\n");
    }

    return ready;
}

// Parse the ABI explicitly: timeval uses microseconds, timespec nanoseconds.
// Round positive fractions up to the scheduler's millisecond resolution.
static int ParseWaitTimeout(uint64_t address, bool nanoseconds, uint64_t* timeoutMs)
{
    *timeoutMs = ~0ULL;
    if (!address) return 0;
    int64_t value[2];
    if (!CopyFromUser(value, address, sizeof(value))) return -EFAULT;
    int64_t sec = value[0], fraction = value[1];
    int64_t scale = nanoseconds ? 1000000000LL : 1000000LL;
    if (sec < 0 || fraction < 0 || fraction >= scale) return -EINVAL;
    uint64_t unitsPerMs = static_cast<uint64_t>(scale / 1000);
    uint64_t partial = (static_cast<uint64_t>(fraction) + unitsPerMs - 1) / unitsPerMs;
    *timeoutMs = static_cast<uint64_t>(sec) > (~0ULL - 1 - partial) / 1000
        ? ~0ULL - 1 : static_cast<uint64_t>(sec) * 1000 + partial;
    return 0;
}

static int64_t PollUser(uint64_t fdsAddr, uint64_t nfds, uint64_t timeoutMs)
{
    if (nfds > MAX_FDS) return -EINVAL;
    uint64_t bytes = nfds * sizeof(pollfd);
    if (bytes && (!UserBufferReadable(fdsAddr, bytes) || !UserBufferWritable(fdsAddr, bytes)))
        return -EFAULT;
    pollfd fds[MAX_FDS];
    if (bytes && !CopyFromUser(fds, fdsAddr, bytes)) return -EFAULT;
    int64_t result = PollWait(fds, nfds, timeoutMs);
    // One checked copy of the whole array: fd and events are written back
    // unchanged from the snapshot taken above, revents with the results.
    if (result >= 0 && bytes && !CopyToUser(fdsAddr, fds, bytes)) return -EFAULT;
    return result;
}

int64_t sys_poll(uint64_t fdsAddr, uint64_t nfds, uint64_t timeout,
                uint64_t, uint64_t, uint64_t)
{
    int32_t milliseconds = static_cast<int32_t>(timeout);
    return PollUser(fdsAddr, nfds, milliseconds < 0 ? ~0ULL : static_cast<uint64_t>(milliseconds));
}

int64_t sys_ppoll(uint64_t fdsAddr, uint64_t nfds, uint64_t tspecAddr,
                 uint64_t sigmaskAddr, uint64_t sigsetsize, uint64_t)
{
    uint64_t timeoutMs;
    int error = ParseWaitTimeout(tspecAddr, true, &timeoutMs);
    if (!error) error = InstallWaitSignalMask(sigmaskAddr, sigsetsize);
    return error ? error : PollUser(fdsAddr, nfds, timeoutMs);
}

// ---------------------------------------------------------------------------
// select/pselect share poll readiness and wait registration, including the
// eventfd notifications used by threaded DNS resolvers.
// ---------------------------------------------------------------------------

static int64_t SelectWait(uint64_t nfds, uint64_t readfdsAddr, uint64_t writefdsAddr,
                          uint64_t exceptfdsAddr, uint64_t timeoutAddr, bool nanoseconds)
{
    if (nfds > MAX_FDS) return -EINVAL;
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    constexpr uint32_t WORDS = (MAX_FDS + 63) / 64;
    uint64_t inputs[3][WORDS] = {}, outputs[3][WORDS] = {};
    uint64_t addresses[3] = {readfdsAddr, writefdsAddr, exceptfdsAddr};
    uint64_t bytes = ((nfds + 63) / 64) * sizeof(uint64_t);
    for (uint32_t set = 0; set < 3; ++set) {
        if (!addresses[set] || !bytes) continue;
        if (!UserBufferWritable(addresses[set], bytes) ||
            !CopyFromUser(inputs[set], addresses[set], bytes)) return -EFAULT;
    }
    uint64_t timeoutMs;
    int error = ParseWaitTimeout(timeoutAddr, nanoseconds, &timeoutMs);
    if (error) return error;
    if (timeoutAddr && !nanoseconds && !UserBufferWritable(timeoutAddr, 16)) return -EFAULT;
    pollfd fds[MAX_FDS];
    uint32_t count = 0;
    for (uint32_t fd = 0; fd < nfds; ++fd) {
        uint64_t mask = 1ULL << (fd % 64);
        uint32_t word = fd / 64;
        short events = 0;
        if (inputs[0][word] & mask) events |= POLLIN;
        if (inputs[1][word] & mask) events |= POLLOUT;
        if (inputs[2][word] & mask) events |= POLLPRI;
        if (!events) continue;
        FdRef ref(proc, fd);
        if (!ref.get() || ref.get()->type == FdType::None) return -EBADF;
        fds[count++] = {static_cast<int>(fd), events, 0};
    }
    uint64_t started = ClockNowNs(1);
    int64_t result = PollWait(fds, count, timeoutMs, true);
    if (timeoutAddr && !nanoseconds) {
        uint64_t elapsed = (ClockNowNs(1) - started) / 1000000ULL;
        uint64_t remaining = elapsed < timeoutMs ? timeoutMs - elapsed : 0;
        const int64_t value[2] = {static_cast<int64_t>(remaining / 1000),
                                  static_cast<int64_t>((remaining % 1000) * 1000)};
        if (!CopyToUser(timeoutAddr, value, sizeof(value))) return -EFAULT;
    }
    if (result < 0) return result;
    int readyBits = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (fds[i].revents & POLLNVAL) return -EBADF;
        uint32_t fd = static_cast<uint32_t>(fds[i].fd), word = fd / 64;
        uint64_t mask = 1ULL << (fd % 64);
        const short conditions[3] = {POLLIN | POLLHUP | POLLERR, POLLOUT | POLLERR, POLLPRI};
        for (uint32_t set = 0; set < 3; ++set) {
            if ((inputs[set][word] & mask) && (fds[i].revents & conditions[set])) {
                outputs[set][word] |= mask;
                ++readyBits;
            }
        }
    }
    for (uint32_t set = 0; set < 3; ++set) {
        if (!addresses[set] || !bytes) continue;
        if (!CopyToUser(addresses[set], outputs[set], bytes)) return -EFAULT;
    }
    return readyBits;
}

int64_t sys_select(uint64_t nfds, uint64_t readfdsAddr, uint64_t writefdsAddr,
                  uint64_t exceptfdsAddr, uint64_t timeoutAddr, uint64_t)
{
    return SelectWait(nfds, readfdsAddr, writefdsAddr, exceptfdsAddr, timeoutAddr, false);
}

int64_t sys_pselect6(uint64_t nfds, uint64_t readfdsAddr, uint64_t writefdsAddr,
                   uint64_t exceptfdsAddr, uint64_t timeoutAddr, uint64_t sigmaskAddr)
{
    if (sigmaskAddr) {
        // The sixth Linux argument points to {sigset pointer, sigset size},
        // rather than directly to a sigset as in ppoll/epoll_pwait.
        struct MaskArgument { uint64_t address; uint64_t size; } argument;
        if (!CopyFromUser(&argument, sigmaskAddr, sizeof(argument))) return -EFAULT;
        int error = InstallWaitSignalMask(argument.address, argument.size);
        if (error) return error;
    }
    return SelectWait(nfds, readfdsAddr, writefdsAddr, exceptfdsAddr, timeoutAddr, true);
}

} // namespace brook
