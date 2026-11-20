// syscall_timerfd.cpp -- timerfd objects and timerfd_create/settime/gettime.
//
// Split from syscall.cpp.
// Adapted from Enkel OS (IanNorris/Enkel, MIT license).

#include "user_copy.h"
#include "syscall_internal.h"
#include "syscall_timerfd.h"
#include "syscall_time.h"
#include "syscall_poll.h"
#include "syscall_handlers.h"

namespace brook {

// timerfd constants and data — defined here so sys_read/sys_poll can use them
static constexpr int TFD_NONBLOCK     = 0x800;
static constexpr int TFD_CLOEXEC      = 0x80000;
static constexpr int TFD_TIMER_ABSTIME = 1;
uint64_t ClockNowNs(uint64_t clockid);
uint64_t WaitWakeupTick(uint64_t deadlineNs, uint64_t safetyMs);
bool TimerFdReady(TimerFdData* timer) {
    TimerFdGuard guard(timer);
    timer->state.refresh(ClockNowNs(timer->clockId));
    return timer->state.pending != 0;
}
// A selected-clock deadline is never a LAPIC tick or a monotonic timestamp.
uint64_t TimerFdDeadline(TimerFdData* timer, uint64_t* generation) {
    TimerFdGuard guard(timer);
    const uint64_t now = ClockNowNs(timer->clockId);
    timer->state.refresh(now);
    if (generation) *generation = __atomic_load_n(&timer->rearmGeneration, __ATOMIC_RELAXED);
    const uint64_t mono = ClockNowNs(1);
    if (timer->state.pending) return mono;
    if (!timer->state.armed) return ~0ULL;
    const uint64_t remaining = timer->state.deadline - now;
    return remaining > ~0ULL - mono ? ~0ULL : mono + remaining;
}
static void TimerFdWakeLocked(TimerFdData* timer) {
    for (auto* n = timer->waiters; n; n = n->nextTimer) WakeProcess(n->process);
}
void TimerFdHandleRef(void* handle) { TimerFdRef(static_cast<TimerFdData*>(handle)); }

// ---------------------------------------------------------------------------
// timerfd — timerfd_create / timerfd_settime / timerfd_gettime
// ---------------------------------------------------------------------------
//
// Used by Wayland clients for frame pacing and animation timers.
// We implement a simple periodic/one-shot timer using LAPIC tick count.

static constexpr int CLOCK_REALTIME  = 0;
static constexpr int CLOCK_MONOTONIC = 1;

struct ITimerSpec {
    struct {
        int64_t tv_sec;
        int64_t tv_nsec;
    } it_interval; // period (0 = one-shot)
    struct {
        int64_t tv_sec;
        int64_t tv_nsec;
    } it_value;    // initial expiry (0 = disarmed)
};

int64_t sys_timerfd_create(uint64_t clockid, uint64_t flags,
                            uint64_t, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EMFILE;

    if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC)
        return -EINVAL;

    if (flags & ~(TFD_NONBLOCK | TFD_CLOEXEC)) return -EINVAL;
    auto* tfd = static_cast<TimerFdData*>(kmalloc(sizeof(TimerFdData)));
    if (!tfd) return -ENOMEM;
    __builtin_memset(tfd, 0, sizeof(*tfd));
    tfd->clockId = static_cast<int>(clockid);
    tfd->refCount = 1;

    // Published complete with O_NONBLOCK and FD_CLOEXEC (L-57).
    int fd = FdAlloc(proc, FdType::TimerFd, tfd, (flags & TFD_NONBLOCK) ? 0x800u : 0u, 0,
                     (flags & TFD_CLOEXEC) ? 1 : 0);
    if (fd < 0) { kfree(tfd); return fd; }

    return fd;
}

static ITimerSpec TimerFdSnapshotLocked(TimerFdData* timer) {
    const uint64_t now = ClockNowNs(timer->clockId);
    timer->state.refresh(now);
    const uint64_t remaining = timer->state.armed ? timer->state.deadline - now : 0;
    ITimerSpec value{};
    value.it_value.tv_sec = remaining / 1000000000ULL;
    value.it_value.tv_nsec = remaining % 1000000000ULL;
    value.it_interval.tv_sec = timer->state.interval / 1000000000ULL;
    value.it_interval.tv_nsec = timer->state.interval % 1000000000ULL;
    return value;
}
int64_t sys_timerfd_settime(uint64_t fd, uint64_t flagsVal,
                             uint64_t newValAddr, uint64_t oldValAddr,
                             uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef pin(proc, static_cast<int>(fd));
    auto* fde = pin.get();
    if (!fde) return -EBADF;
    if (fde->type != FdType::TimerFd || !fde->handle) return -EINVAL;
    // Wall-clock setting/cancellation is not implemented. Reject unsupported
    // flags rather than claiming CANCEL_ON_SET behavior.
    if (flagsVal & ~static_cast<uint64_t>(TFD_TIMER_ABSTIME)) return -EINVAL;
    ITimerSpec value{};
    if (!CopyFromUser(&value, newValAddr, sizeof(value))) return -EFAULT;
    uint64_t initial, interval;
    if (!brook::TimerFdNanoseconds(value.it_value.tv_sec, value.it_value.tv_nsec, &initial) ||
        !brook::TimerFdNanoseconds(value.it_interval.tv_sec, value.it_interval.tv_nsec, &interval))
        return -EINVAL;
    auto* timer = static_cast<TimerFdData*>(fde->handle);
    TimerFdGuard guard(timer);
    const uint64_t now = ClockNowNs(timer->clockId);
    const ITimerSpec old = TimerFdSnapshotLocked(timer);
    if (oldValAddr && !CopyToUser(oldValAddr, &old, sizeof(old))) return -EFAULT;
    timer->state.interval = interval;
    timer->state.pending = 0;
    timer->state.armed = initial != 0;
    timer->state.deadline = !initial ? 0 : (flagsVal & TFD_TIMER_ABSTIME) ? initial : TimeoutAddSafe(now, initial);
    __atomic_fetch_add(&timer->rearmGeneration, 1, __ATOMIC_RELEASE);
    TimerFdWakeLocked(timer);
    return 0;
}

int64_t sys_timerfd_gettime(uint64_t fd, uint64_t currValAddr,
                             uint64_t, uint64_t, uint64_t, uint64_t)
{
    Process* proc = ProcessCurrent();
    if (!proc) return -EBADF;
    FdRef pin(proc, static_cast<int>(fd));
    auto* fde = pin.get();
    if (!fde) return -EBADF;
    if (fde->type != FdType::TimerFd || !fde->handle) return -EINVAL;
    auto* timer = static_cast<TimerFdData*>(fde->handle);
    TimerFdGuard guard(timer);
    const ITimerSpec value = TimerFdSnapshotLocked(timer);
    return CopyToUser(currValAddr, &value, sizeof(value)) ? 0 : -EFAULT;
}

} // namespace brook
