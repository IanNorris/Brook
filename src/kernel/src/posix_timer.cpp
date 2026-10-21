#include "apic.h"
#include "posix_timer.h"
#include "process.h"
#include "scheduler.h"
#include "spinlock.h"
#include "kvmclock.h"
#include "rtc.h"
#include "timeout_value.h"

namespace brook {
static_assert(sizeof(SignalInfo) == 128);
static_assert(offsetof(SignalInfo, _data) == 16);
namespace {
constexpr unsigned TimerLimit = 128;
constexpr uint64_t Billion = 1000000000ULL;
constexpr int MaxOverrun = 0x7fffffff;
struct Timer {
    int32_t id;
    int clock, basis, notify, signo;
    uint16_t owner, target;
    uint64_t ownerIncarnation, targetIncarnation;
    uint64_t value, deadline, interval, queuedAt;
    int overrun, deliveredOverrun;
    bool used, armed, pending, notified;
};
Timer timers[TimerLimit] = {};
IrqSpinLock timerLock = {};
uint32_t usedCount = 0, pendingCount = 0;
// Never reuse a kernel timer ID, including after deletion. Refuse exhaustion
// rather than alias a stale ID. IDs remain positive signed 32-bit integers.
uint32_t nextId = 1;
uint64_t nextNotification = 1;
uint64_t Now(int clock)
{
    // Realtime's seconds and fraction must share the RTC driver's immutable
    // epoch. Pvclock's fractional phase is unrelated to that epoch.
    if (clock == 0) return RtcNowNs();
    return KvmClockAvailable() ? KvmClockReadNs() : ApicTickCount() * 1000000ULL;
}
uint64_t OwnerIncarnation(Process* p)
{
    return p->isThread ? p->leaderIncarnation : p->incarnation;
}
bool Owned(const Timer& t, Process* p)
{
    return t.used && t.owner == p->tgid && t.ownerIncarnation == OwnerIncarnation(p);
}
bool IgnoredUnblocked(const Timer& t, Process* p)
{
    if (p->sigMask & (1ULL << (t.signo - 1))) return false;
    const auto action = ReadSignalAction(p->tgid,t.signo - 1);
    return action.handler == 1 || (action.handler == 0 &&
        (t.signo == 17 || t.signo == 23 || t.signo == 28));
}
bool Recipient(const Timer& t, Process* p)
{
    return Owned(t, p) && (t.notify != 4 ||
        (t.target == p->pid && t.targetIncarnation == p->incarnation));
}
Timer* Lookup(Process* p, int32_t id)
{
    for (auto& t : timers) if (t.id == id && Owned(t, p)) return &t;
    return nullptr;
}
bool ToNs(const PosixTimespec& ts, uint64_t* ns)
{
    return TimeoutNanoseconds(ts.sec, ts.nsec, ns);
}
PosixTimespec FromNs(uint64_t ns)
{
    return {(int64_t)(ns / Billion), (int64_t)(ns % Billion)};
}
void Advance(Timer& t, uint64_t now)
{
    if (!t.armed || now < t.deadline) return;
    uint64_t expirations = 1;
    if (t.interval) {
        uint64_t elapsed = now - t.deadline;
        uint64_t quotient = elapsed / t.interval;
        // Avoid both quotient+1 and deadline+interval*n overflow.
        expirations = quotient == ~uint64_t{0} ? ~uint64_t{0} : quotient + 1;
        uint64_t untilNext = t.interval - elapsed % t.interval;
        if (now >= TimeoutMaxNs) t.armed = false;
        else t.deadline = TimeoutAddSafe(now, untilNext);
    } else t.armed = false;
    if (t.notify == 1) return; // SIGEV_NONE
    uint64_t extra = t.pending ? expirations : expirations - 1;
    uint64_t available = (uint64_t)(MaxOverrun - (t.pending ? t.overrun : 0));
    t.overrun = extra >= available ? MaxOverrun :
        (t.pending ? t.overrun : 0) + (int)extra;
    if (!t.pending) {
        t.notified = false;
        t.queuedAt = nextNotification++;
        __atomic_add_fetch(&pendingCount, 1U, __ATOMIC_RELEASE);
    }
    t.pending = true;
}
void ReadValue(Timer& t, uint64_t now, PosixItimerspec* result)
{
    Advance(t, now);
    result->interval = FromNs(t.interval);
    result->value = FromNs(t.armed && t.deadline > now ? t.deadline - now : 0);
}
}

int PosixTimerCreate(Process* p, int clock, const PosixSigevent* ev, int32_t* id)
{
    if (!p) return -3;
    if (clock != 0 && clock != 1) return -22;
    PosixSigevent event{};
    if (ev) event = *ev;
    else { event.notify = 0; event.signo = 14; }
    if (event.notify != 0 && event.notify != 1 && event.notify != 4) return -22;
    if (event.notify != 1 && (event.signo < 1 || event.signo > 64)) return -22;
    // Stop/continue/uncatchable notifications require IRQ-return job-control
    // delivery, which this syscall-return signal engine does not yet provide.
    if (event.notify != 1 && (event.signo == 9 || event.signo == 18 ||
        event.signo == 19 || event.signo == 20 || event.signo == 21 || event.signo == 22)) return -95;
    uint64_t targetIncarnation = 0;
    if (event.notify == 4 &&
        !SchedulerTimerTargetIncarnation(p, event.targetTid, &targetIncarnation)) return -22;
    uint64_t flags = IrqSpinLockAcquire(&timerLock);
    if (p->state == ProcessState::Terminated ||
        __atomic_load_n(&p->tgidExiting, __ATOMIC_ACQUIRE)) {
        IrqSpinLockRelease(&timerLock, flags); return -3;
    }
    if (nextId > 0x7fffffffU) { IrqSpinLockRelease(&timerLock, flags); return -11; }
    for (auto& t : timers) {
        if (t.used) continue;
        t = {};
        t.id = (int32_t)nextId++;
        t.owner = p->tgid;
        t.ownerIncarnation = OwnerIncarnation(p);
        t.target = event.notify == 4 ? (uint16_t)event.targetTid : 0;
        t.targetIncarnation = targetIncarnation;
        t.clock = clock; t.basis = clock; t.notify = event.notify; t.signo = event.signo;
        t.value = ev ? event.value : (uint64_t)t.id;
        t.used = true;
        __atomic_add_fetch(&usedCount, 1U, __ATOMIC_RELEASE);
        *id = t.id;
        IrqSpinLockRelease(&timerLock, flags);
        return 0;
    }
    IrqSpinLockRelease(&timerLock, flags);
    return -11;
}
int PosixTimerSet(Process* p, int32_t id, unsigned flags,
                  const PosixItimerspec& value, PosixItimerspec* old)
{
    uint64_t initial, interval;
    if (!ToNs(value.value, &initial) || !ToNs(value.interval, &interval)) return -22;
    uint64_t lockFlags = IrqSpinLockAcquire(&timerLock);
    Timer* t = Lookup(p, id);
    if (!t) { IrqSpinLockRelease(&timerLock, lockFlags); return -22; }
    // Linux ignores unknown flag bits. Relative realtime timers use the
    // monotonic clock; absolute realtime timers track the realtime clock.
    int basis = t->clock == 0 && !(flags & 1) ? 1 : t->clock;
    uint64_t now = Now(basis), deadline = initial;
    if (initial && !(flags & 1)) deadline = TimeoutAddSafe(now, initial);
    if (old) ReadValue(*t, Now(t->basis), old);
    // Match Linux: disarming clears the interval and settime resets overrun
    // accounting. A notification already pending is not erased here.
    t->interval = initial ? interval : 0;
    t->overrun = 0; t->deliveredOverrun = 0;
    t->basis = basis; t->deadline = deadline; t->armed = initial != 0;
    IrqSpinLockRelease(&timerLock, lockFlags);
    return 0;
}
int PosixTimerGet(Process* p, int32_t id, PosixItimerspec* result)
{
    uint64_t flags = IrqSpinLockAcquire(&timerLock);
    Timer* t = Lookup(p, id);
    if (t) ReadValue(*t, Now(t->basis), result);
    IrqSpinLockRelease(&timerLock, flags);
    return t ? 0 : -22;
}
int PosixTimerOverrun(Process* p, int32_t id)
{
    uint64_t flags = IrqSpinLockAcquire(&timerLock);
    Timer* t = Lookup(p, id);
    int result = t ? t->deliveredOverrun : -22;
    IrqSpinLockRelease(&timerLock, flags);
    return result;
}
int PosixTimerDelete(Process* p, int32_t id)
{
    uint64_t flags = IrqSpinLockAcquire(&timerLock);
    Timer* t = Lookup(p, id);
    if (t) {
        if (t->pending) __atomic_sub_fetch(&pendingCount, 1U, __ATOMIC_RELEASE);
        t->pending = false;
        t->used = false; // deletion discards this timer's queued notification
        __atomic_sub_fetch(&usedCount, 1U, __ATOMIC_RELEASE);
    }
    IrqSpinLockRelease(&timerLock, flags);
    return t ? 0 : -22;
}
uint64_t PosixTimerPending(Process* p)
{
    // Most syscalls have no pending timer signals. Avoid a global lock on
    // that hot path; an expiry racing this check also publishes a wake latch.
    if (!__atomic_load_n(&pendingCount, __ATOMIC_ACQUIRE)) return 0;
    uint64_t result = 0;
    uint64_t flags = IrqSpinLockAcquire(&timerLock);
    for (auto& t : timers)
        if (t.pending && Recipient(t, p) && !IgnoredUnblocked(t, p))
            result |= 1ULL << (t.signo - 1);
    IrqSpinLockRelease(&timerLock, flags);
    return result;
}
int PosixTimerDequeue(Process* p, uint64_t mask, SignalInfo* info)
{
    uint64_t flags = IrqSpinLockAcquire(&timerLock);
    Timer* chosen = nullptr;
    for (auto& t : timers)
        if (t.pending && Recipient(t, p) && (mask & (1ULL << (t.signo - 1))) &&
            (!chosen || t.signo < chosen->signo ||
             (t.signo == chosen->signo && t.queuedAt < chosen->queuedAt))) chosen = &t;
    int result = 0;
    if (chosen) {
        auto& t = *chosen;
        Advance(t, Now(t.basis));
        *info = {};
        info->si_signo = t.signo; info->si_code = -2; // SI_TIMER
        // SignalInfo union starts at byte 16 on Linux x86-64.
        __builtin_memcpy(info->_data, &t.id, 4);
        __builtin_memcpy(info->_data + 4, &t.overrun, 4);
        __builtin_memcpy(info->_data + 8, &t.value, 8);
        t.deliveredOverrun = t.overrun;
        t.overrun = 0; t.pending = false; t.notified = false;
        __atomic_sub_fetch(&pendingCount, 1U, __ATOMIC_RELEASE);
        result = t.signo;
    }
    IrqSpinLockRelease(&timerLock, flags);
    return result;
}
void PosixTimersTickLocked(Process* const* processes, uint32_t count)
{
    if (!__atomic_load_n(&usedCount, __ATOMIC_ACQUIRE)) return;
    uint64_t monotonic = Now(1), realtime = Now(0);
    uint64_t flags = IrqSpinLockAcquire(&timerLock);
    for (auto& t : timers) {
        if (!t.used) continue;
        Advance(t, t.basis == 0 ? realtime : monotonic);
        if (!t.pending || t.notified) continue;
        // All process pointers are protected by the caller's registry lock.
        // Wake synchronous waiters even when the selected signal is blocked.
        bool recipient = false;
        for (uint32_t i = 0; i < count; ++i) {
            Process* p = processes[i];
            if (p->state != ProcessState::Terminated && Recipient(t, p) &&
                !IgnoredUnblocked(t, p)) {
                recipient = true;
                __atomic_store_n(&p->pendingWakeup, 1, __ATOMIC_RELEASE);
            }
        }
        if (!recipient) {
            // An ignored, unblocked signal must not interrupt nanosleep or
            // other waits. Blocked signals remain available to sigtimedwait.
            t.pending = false; t.overrun = 0;
            __atomic_sub_fetch(&pendingCount, 1U, __ATOMIC_RELEASE);
        }
        t.notified = true;
    }
    IrqSpinLockRelease(&timerLock, flags);
}
void PosixTimersCleanup(Process* p, bool exec)
{
    uint64_t flags = IrqSpinLockAcquire(&timerLock);
    for (auto& t : timers) {
        if (!t.used) continue;
        bool remove = (exec || (!p->isThread &&
            __atomic_load_n(&p->asLiveThreads, __ATOMIC_ACQUIRE) == 0)) && Owned(t, p);
        bool targetGone = t.notify == 4 && t.target == p->pid && t.targetIncarnation == p->incarnation;
        if (remove || targetGone) {
            if (t.pending) __atomic_sub_fetch(&pendingCount, 1U, __ATOMIC_RELEASE);
            t.armed = false; t.pending = false; t.notified = false;
        }
        if (remove) {
            t.used = false;
            __atomic_sub_fetch(&usedCount, 1U, __ATOMIC_RELEASE);
        }
    }
    IrqSpinLockRelease(&timerLock, flags);
}
}
