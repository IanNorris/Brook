// syscall_time.cpp -- clock, sleep and POSIX timer syscalls.
//
// Split from syscall.cpp (see docs/agent/syscall-split-plan.md).
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "posix_timer.h"
#include "timeout_value.h"
#include "user_access_scope.h"
#include "rtc.h"
#include "tsc_clock.h"
#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_time.h"
#include "syscall_handlers.h"

namespace brook {

// ---------------------------------------------------------------------------
// sys_clock_gettime (228) / sys_clock_getres (229) / sys_gettimeofday (96)
// ---------------------------------------------------------------------------

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

// Coarse clocks retain their corresponding epoch. Higher precision is safe;
// returning uptime for REALTIME_COARSE makes absolute realtime waits expire.
// CLOCK_REALTIME=0, REALTIME_COARSE=5; MONOTONIC=1, RAW=4, COARSE=6, BOOTTIME=7.
// Monotonic: KVM pvclock, else the invariant TSC, else delivered LAPIC ticks
// (tsc_clock.h). Realtime is the boot RTC epoch plus that elapsed time.
uint64_t ClockNowNs(uint64_t clockid)
{
    if (clockid == 0 || clockid == 5 || clockid == 8) return RtcNowNs();
    return ElapsedClockNs();
}

int64_t sys_clock_gettime(uint64_t clockid, uint64_t tsAddr, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    UserAccessScope userAccess(ProcessCurrent());
    if (clockid > 9) return -EINVAL; // No TAI/dynamic CPU clock support yet.
    timespec value{};auto* ts=&value;
    uint64_t ns = 0;
    if (clockid == 2 || clockid == 3) {
        if (!SchedulerCpuTime(ProcessCurrent(), clockid == 2, &ns)) return -EINVAL;
    } else ns = ClockNowNs(clockid);
    ts->tv_sec = static_cast<int64_t>(ns / 1000000000ULL);
    ts->tv_nsec = static_cast<int64_t>(ns % 1000000000ULL);
    return CopyToUser(tsAddr,&value,sizeof(value))?0:-EFAULT;
}

int64_t sys_clock_getres(uint64_t clockid, uint64_t tsAddr, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    if (clockid > 9) return -EINVAL;
    if (!tsAddr) return 0;
    timespec value{};auto* ts=&value;

    ts->tv_sec = 0;
    ts->tv_nsec = clockid == 2 || clockid == 3 ? 1000000 : (ElapsedClockNsResolution() ? 1 : 1000000);
    return CopyToUser(tsAddr,&value,sizeof(value))?0:-EFAULT;
}

int64_t sys_gettimeofday(uint64_t tvAddr, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    timeval value{};auto* tv=&value;

    const uint64_t ns = RtcNowNs();
    tv->tv_sec = static_cast<int64_t>(ns / 1000000000ULL);
    tv->tv_usec = static_cast<int64_t>((ns / 1000) % 1000000ULL);
    return CopyToUser(tvAddr,&value,sizeof(value))?0:-EFAULT;
}

// ---------------------------------------------------------------------------
// sys_time (201) — returns seconds since epoch
// ---------------------------------------------------------------------------

int64_t sys_time(uint64_t tloc, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t)
{
    uint64_t epoch = RtcNow();
    if (tloc) {
        if(!CopyToUser(tloc,&epoch,sizeof(epoch)))return -EFAULT;
    }
    return static_cast<int64_t>(epoch);
}

// ---------------------------------------------------------------------------
// sys_nanosleep (35) / sys_clock_nanosleep (230)
// ---------------------------------------------------------------------------

int64_t sys_nanosleep(uint64_t reqAddr, uint64_t remAddr, uint64_t,
                       uint64_t, uint64_t, uint64_t)
{
    timespec req;
    if (!CopyFromUser(&req, reqAddr, sizeof(req))) return -EFAULT;
    uint64_t duration;
    if (!TimeoutNanoseconds(req.tv_sec, req.tv_nsec, &duration)) return -EINVAL;
    const uint64_t deadline = TimeoutAddSafe(ClockNowNs(1), duration);

    Process* proc = ProcessCurrent();
    if (!proc) return -ESRCH;
    uint64_t now = ClockNowNs(1);
    while (now < deadline)
    {
        if (HasPendingSignals())
        {
            if (remAddr)
            {
                uint64_t remaining = deadline - now;
                timespec rem{};
                rem.tv_sec = static_cast<int64_t>(remaining / 1000000000ULL);
                rem.tv_nsec = static_cast<int64_t>(remaining % 1000000000ULL);
                if (!CopyToUser(remAddr, &rem, sizeof(rem))) return -EFAULT;
            }
            return -EINTR;
        }
        // Round positive durations UP to a scheduler tick. Recheck the
        // monotonic deadline after wakeup, since a tick can arrive early or
        // another event can unblock this process before the sleep expires.
        uint64_t remaining = deadline - now;
        uint64_t ticks = remaining / 1000000ULL + (remaining % 1000000ULL != 0);
        proc->wakeupTick = ApicTickCount() + ticks;
        SchedulerBlockInterruptible(proc);
        now = ClockNowNs(1);
    }

    if (remAddr)
    {
        // Best effort, as before: a completed sleep never fails on rem.
        const timespec rem{};
        (void)CopyToUser(remAddr, &rem, sizeof(rem));
    }
    return 0;
}

int64_t sys_clock_nanosleep(uint64_t clockid, uint64_t flags, uint64_t reqAddr,
                             uint64_t remAddr, uint64_t, uint64_t)
{
    if (clockid != 0 && clockid != 1 && clockid != 7) return -EINVAL;
    if (flags & ~1ULL) return -EINVAL;
    timespec req;
    if (!CopyFromUser(&req, reqAddr, sizeof(req))) return -EFAULT;
    uint64_t requested;
    if (!TimeoutNanoseconds(req.tv_sec, req.tv_nsec, &requested)) return -EINVAL;
    if (flags & 1) { // TIMER_ABSTIME: convert from the requested clock domain.
        const uint64_t deadline = requested;
        uint64_t now = ClockNowNs(clockid);
        if (deadline <= now) return 0;
        Process* proc = ProcessCurrent();
        if (!proc) return -EINVAL;
        while (now < deadline) {
            if (HasPendingSignals()) return -EINTR;
            const uint64_t remaining = deadline - now;
            const uint64_t ticks = remaining / 1000000ULL + (remaining % 1000000ULL != 0);
            proc->wakeupTick = ApicTickCount() + ticks;
            SchedulerBlockInterruptible(proc);
            // LAPIC ticks and unrelated wakeups are scheduling hints, not
            // evidence that the selected clock's absolute deadline expired.
            now = ClockNowNs(clockid);
        }
        return 0;
    }
    return sys_nanosleep(reqAddr, remAddr, 0, 0, 0, 0);
}

// POSIX timer ABI wrappers. All user memory is validated/copied outside the
// timer lock; the implementation never follows user pointers while locked.
int64_t sys_timer_create(uint64_t clock, uint64_t eventAddr, uint64_t idAddr,
                         uint64_t, uint64_t, uint64_t)
{
    if (!UserBufferWritable(idAddr, sizeof(int32_t))) return -EFAULT;
    PosixSigevent event{};
    if (eventAddr && !CopyFromUser(&event, eventAddr, sizeof(event))) return -EFAULT;
    int32_t id;
    int result = PosixTimerCreate(ProcessCurrent(), (int)clock, eventAddr ? &event : nullptr, &id);
    // A raced unmap of the id buffer must not leak the new timer.
    if (!result && !CopyToUser(idAddr, &id, sizeof(id))) {
        (void)PosixTimerDelete(ProcessCurrent(), id);
        return -EFAULT;
    }
    return result;
}
int64_t sys_timer_settime(uint64_t id, uint64_t flags, uint64_t valueAddr,
                          uint64_t oldAddr, uint64_t, uint64_t)
{
    PosixItimerspec value, old;
    if (!valueAddr) return -EINVAL;
    if (!CopyFromUser(&value, valueAddr, sizeof(value))) return -EFAULT;
    int result = PosixTimerSet(ProcessCurrent(), (int32_t)id, (unsigned)flags, value, oldAddr ? &old : nullptr);
    if (!result && oldAddr && !CopyToUser(oldAddr, &old, sizeof(old))) return -EFAULT;
    return result;
}
int64_t sys_timer_gettime(uint64_t id, uint64_t valueAddr, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    PosixItimerspec value;
    if (!UserBufferWritable(valueAddr, sizeof(value))) return -EFAULT;
    int result = PosixTimerGet(ProcessCurrent(), (int32_t)id, &value);
    if (!result && !CopyToUser(valueAddr, &value, sizeof(value))) return -EFAULT;
    return result;
}
int64_t sys_timer_getoverrun(uint64_t id, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t)
{
    return PosixTimerOverrun(ProcessCurrent(), (int32_t)id);
}
int64_t sys_timer_delete(uint64_t id, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t)
{
    return PosixTimerDelete(ProcessCurrent(), (int32_t)id);
}

} // namespace brook
