#include "apic.h"
#include "posix_timer.h"
#include "process.h"
#include "scheduler.h"
#include "spinlock.h"
#include "kvmclock.h"
#include "tsc_clock.h"
#include "rtc.h"
#include "timeout_value.h"
#include "real_interval_timer.h"

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
    return ElapsedClockNs();
}
uint64_t OwnerIncarnation(Process* p)
{
    return p->isThread ? p->leaderIncarnation : p->incarnation;
}
bool Owned(const Timer& t, Process* p)
{
    return t.used && t.owner == p->tgid && t.ownerIncarnation == OwnerIncarnation(p);
}
bool IgnoredUnblockedSignal(int signo, Process* p)
{
    if (p->sigMask & (1ULL << (signo - 1))) return false;
    const auto action = ReadSignalAction(p->tgid,signo - 1);
    return action.handler == 1 || (action.handler == 0 &&
        (signo == 17 || signo == 23 || signo == 28));
}
bool IgnoredUnblocked(const Timer& t, Process* p)
{
    return IgnoredUnblockedSignal(t.signo, p);
}

// ---- setitimer/getitimer state (see posix_timer.h) ----
// All fields are guarded by timerLock. pendingCount counts each pending
// interval timer once, so the PosixTimerPending fast path stays one load.
constexpr int ItimerSignal[3] = {14, 26, 27}; // SIGALRM, SIGVTALRM, SIGPROF
constexpr int SiKernel = 0x80;
struct CpuItimer { uint64_t remaining, interval; bool pending; };
struct GroupItimers {
    uint64_t owner;          // group incarnation (OwnerIncarnation)
    RealIntervalTimer real;
    CpuItimer cpu[2];        // VIRTUAL, PROF
    uint8_t countedPending;  // bit i: itimer i contributes to pendingCount
    uint8_t countedArmed;    // bit i: contributes to the armed counters
    bool notified;           // recipients woken for the current pending set
};
GroupItimers groupItimers[MAX_PROCESSES] = {};
uint32_t realArmedCount = 0;
uint64_t realNextDeadline = ~uint64_t{0}; // lower bound of armed REAL deadlines
uint32_t itimerUnnotified = 0;            // a CPU itimer became pending off-BSP
// Default-action (terminate) expiries found by the BSP tick. Delivered after
// the registry lock drops, so a CPU-bound process without a handler dies as
// Linux kills it, rather than waiting for a syscall return. BSP-only.
constexpr uint32_t FatalLimit = 16;
struct FatalItimer { Process* target; int signo; };
FatalItimer fatalItimers[FatalLimit];
uint32_t fatalItimerCount = 0;
uint8_t PendingBits(const GroupItimers& g)
{
    return uint8_t((g.real.Pending() ? 1 : 0) | (g.cpu[0].pending ? 2 : 0) |
                   (g.cpu[1].pending ? 4 : 0));
}
// Publish one record's state into the global counters after any change.
void Reconcile(GroupItimers& g)
{
    const uint8_t pending = PendingBits(g);
    const uint8_t armed = uint8_t((g.real.Armed() ? 1 : 0) | (g.cpu[0].remaining ? 2 : 0) |
                                  (g.cpu[1].remaining ? 4 : 0));
    for (unsigned i = 0; i < 3; ++i) {
        const uint8_t bit = uint8_t(1u << i);
        if ((pending & bit) && !(g.countedPending & bit)) {
            __atomic_add_fetch(&pendingCount, 1U, __ATOMIC_RELEASE); g.notified = false;
        } else if (!(pending & bit) && (g.countedPending & bit))
            __atomic_sub_fetch(&pendingCount, 1U, __ATOMIC_RELEASE);
        if ((armed & bit) != (g.countedArmed & bit)) {
            uint32_t* counter = i ? &g_intervalCpuTimersArmed : &realArmedCount;
            if (armed & bit) __atomic_add_fetch(counter, 1U, __ATOMIC_RELEASE);
            else __atomic_sub_fetch(counter, 1U, __ATOMIC_RELEASE);
        }
    }
    g.countedPending = pending; g.countedArmed = armed;
    if (g.real.Armed() && g.real.Deadline() < realNextDeadline)
        __atomic_store_n(&realNextDeadline, g.real.Deadline(), __ATOMIC_RELEASE);
}
void ResetItimers(GroupItimers& g, uint64_t owner)
{
    g.real = RealIntervalTimer{}; g.cpu[0] = g.cpu[1] = CpuItimer{};
    Reconcile(g); g.owner = owner; g.notified = false;
}
bool ItimerMember(const GroupItimers& g, uint32_t group, Process* p)
{
    return p->tgid == group && OwnerIncarnation(p) == g.owner;
}
// The calling group's record; claim=true resets a stale (reused-TGID) record.
GroupItimers* ItimersFor(Process* p, bool claim)
{
    if (!p || p->tgid >= MAX_PROCESSES) return nullptr;
    auto& g = groupItimers[p->tgid];
    const uint64_t owner = OwnerIncarnation(p);
    if (g.owner == owner) return &g;
    if (!claim) return nullptr;
    ResetItimers(g, owner);
    return &g;
}
uint64_t ItimerPendingLocked(Process* p)
{
    auto* g = ItimersFor(p, false);
    if (!g || !g->countedPending) return 0;
    uint64_t result = 0;
    for (unsigned i = 0; i < 3; ++i)
        if ((g->countedPending & (1u << i)) && !IgnoredUnblockedSignal(ItimerSignal[i], p))
            result |= 1ULL << (ItimerSignal[i] - 1);
    return result;
}
// Index of the lowest pending itimer whose signal is in mask, or -1.
int ItimerFindPending(Process* p, uint64_t mask, GroupItimers** out)
{
    auto* g = ItimersFor(p, false);
    if (!g || !g->countedPending) return -1;
    for (int i = 0; i < 3; ++i)
        if ((g->countedPending & (1u << i)) && (mask & (1ULL << (ItimerSignal[i] - 1)))) {
            *out = g; return i;
        }
    return -1;
}
int ItimerDequeueLocked(GroupItimers& g, int which, SignalInfo* info)
{
    *info = {};
    info->si_signo = ItimerSignal[which]; info->si_code = SiKernel; // SEND_SIG_PRIV
    if (which == ItimerReal) g.real.SharedAlarmDequeued(Now(1));
    else g.cpu[which - 1].pending = false;
    Reconcile(g);
    return ItimerSignal[which];
}
// BSP tick: expire REAL timers, wake recipients of new expiries and discard
// expiries nobody can receive (ignored and unblocked, or a dead group).
void ItimersTickLocked(Process* const* processes, uint32_t count)
{
    const bool due = __atomic_load_n(&realArmedCount, __ATOMIC_ACQUIRE) &&
        Now(1) >= __atomic_load_n(&realNextDeadline, __ATOMIC_ACQUIRE);
    if (!due && !__atomic_load_n(&itimerUnnotified, __ATOMIC_ACQUIRE)) return;
    uint64_t flags = IrqSpinLockAcquire(&timerLock);
    __atomic_store_n(&itimerUnnotified, 0U, __ATOMIC_RELAXED);
    const uint64_t now = Now(1);
    uint64_t next = ~uint64_t{0};
    for (uint32_t group = 0; group < MAX_PROCESSES; ++group) {
        auto& g = groupItimers[group];
        if (!g.countedArmed && !g.countedPending) continue;
        if (due) g.real.Expire(now);
        Reconcile(g);
        if (g.countedPending && !g.notified) {
            uint8_t received = 0;
            for (uint32_t i = 0; i < count; ++i) {
                Process* p = processes[i];
                if (p->state == ProcessState::Terminated || !ItimerMember(g, group, p)) continue;
                uint8_t wants = 0;
                for (unsigned k = 0; k < 3; ++k) {
                    const uint8_t bit = uint8_t(1u << k);
                    if (!(g.countedPending & bit) || IgnoredUnblockedSignal(ItimerSignal[k], p)) continue;
                    const uint64_t mask = 1ULL << (ItimerSignal[k] - 1);
                    // SIGALRM/SIGVTALRM/SIGPROF default to terminate. Hand an
                    // unblocked SIG_DFL expiry to the ordinary sender (which
                    // requests the group kill) instead of leaving it pending.
                    if (!(p->sigMask & mask) && ReadSignalAction(p->tgid, ItimerSignal[k] - 1).handler == 0 &&
                        !(received & bit) && fatalItimerCount < FatalLimit) {
                        ProcessRef(p); // registry lock held by the caller
                        fatalItimers[fatalItimerCount++] = {p, ItimerSignal[k]};
                        if (k == 0) g.real.Discard(); else g.cpu[k - 1].pending = false;
                        received |= bit;
                        continue;
                    }
                    wants |= bit;
                }
                if (wants) __atomic_store_n(&p->pendingWakeup, 1, __ATOMIC_RELEASE);
                received |= wants;
            }
            // Linux drops an ignored signal at generation; a REAL timer then
            // stays expired (no periodic restart) until reprogrammed.
            if ((g.countedPending & 1) && !(received & 1)) g.real.Discard();
            for (unsigned k = 1; k < 3; ++k)
                if ((g.countedPending & (1u << k)) && !(received & (1u << k))) g.cpu[k - 1].pending = false;
            Reconcile(g);
            g.notified = true;
        }
        if (g.real.Armed() && g.real.Deadline() < next) next = g.real.Deadline();
    }
    __atomic_store_n(&realNextDeadline, next, __ATOMIC_RELEASE);
    IrqSpinLockRelease(&timerLock, flags);
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
    uint16_t globalTarget = 0;
    uint64_t targetIncarnation = 0;
    if (event.notify == 4 &&
        !SchedulerTimerTargetIncarnation(p, event.targetTid, &globalTarget, &targetIncarnation)) return -22;
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
        t.target = globalTarget;
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
    result |= ItimerPendingLocked(p);
    IrqSpinLockRelease(&timerLock, flags);
    return result;
}
static Timer* FindPending(Process* p,uint64_t mask)
{
    Timer* chosen = nullptr;
    for (auto& t : timers)
        if (t.pending && Recipient(t, p) && (mask & (1ULL << (t.signo - 1))) &&
            (!chosen || t.signo < chosen->signo ||
             (t.signo == chosen->signo && t.queuedAt < chosen->queuedAt))) chosen = &t;
    return chosen;
}
static int DequeueTimerLocked(Timer* chosen,SignalInfo* info) {
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
    return result;
}
int PosixTimerDequeue(Process* p, uint64_t mask, SignalInfo* info)
{
    uint64_t flags=IrqSpinLockAcquire(&timerLock);
    int result=DequeueTimerLocked(FindPending(p,mask),info);
    GroupItimers* group=nullptr;
    if(!result) {
        const int which=ItimerFindPending(p,mask,&group);
        if(which>=0)result=ItimerDequeueLocked(*group,which,info);
    }
    IrqSpinLockRelease(&timerLock,flags);return result;
}
bool PosixTimerClaimCaught(Process* p,int signum,SignalInfo* info,KernelSigaction* action)
{
    if(!p || signum<1 || signum>64 || !info || !action)return false;
    const uint64_t flags=IrqSpinLockAcquire(&timerLock);
    auto* chosen=FindPending(p,1ULL<<(signum-1));
    bool consumed=false;
    if(chosen) {
        const auto snapshot=ReadSignalAction(p->tgid,signum-1);
        if(snapshot.handler>1) {*action=snapshot;consumed=DequeueTimerLocked(chosen,info)!=0;}
    } else {
        GroupItimers* group=nullptr;
        const int which=ItimerFindPending(p,1ULL<<(signum-1),&group);
        if(which>=0) {
            const auto snapshot=ReadSignalAction(p->tgid,signum-1);
            if(snapshot.handler>1) {*action=snapshot;consumed=ItimerDequeueLocked(*group,which,info)!=0;}
        }
    }
    IrqSpinLockRelease(&timerLock,flags);return consumed;
}
void PosixTimersTickLocked(Process* const* processes, uint32_t count)
{
    ItimersTickLocked(processes, count);
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
    // Interval timers survive exec; they end with the group: the leader's
    // terminal/destroy path once no siblings remain, or the last sibling's
    // release of a terminated leader (exec=true with a Terminated leader).
    const bool groupGone = !p->isThread &&
        (exec ? p->state == ProcessState::Terminated
              : __atomic_load_n(&p->asLiveThreads, __ATOMIC_ACQUIRE) == 0);
    if (groupGone)
        if (auto* g = ItimersFor(p, false)) ResetItimers(*g, g->owner);
    IrqSpinLockRelease(&timerLock, flags);
}

uint32_t g_intervalCpuTimersArmed = 0;

uint32_t PosixTimersTakeFatal(Process** targets, int* signals, uint32_t capacity)
{
    uint64_t flags = IrqSpinLockAcquire(&timerLock);
    uint32_t n = 0;
    for (; n < fatalItimerCount && n < capacity; ++n) {
        targets[n] = fatalItimers[n].target; signals[n] = fatalItimers[n].signo;
    }
    for (uint32_t i = n; i < fatalItimerCount; ++i) fatalItimers[i - n] = fatalItimers[i];
    fatalItimerCount -= n;
    IrqSpinLockRelease(&timerLock, flags);
    return n;
}

int IntervalTimerSet(Process* p, int which, uint64_t valueNs, uint64_t intervalNs,
                     IntervalTimerValue* old)
{
    if (which < ItimerReal || which > ItimerProf) return -22;
    uint64_t flags = IrqSpinLockAcquire(&timerLock);
    auto* g = ItimersFor(p, true);
    if (!g) { IrqSpinLockRelease(&timerLock, flags); return -3; }
    IntervalTimerValue previous{};
    if (which == ItimerReal) {
        const auto value = g->real.Set(Now(1), valueNs, intervalNs);
        previous = {value.remaining, value.interval};
    } else {
        auto& c = g->cpu[which - 1];
        previous = {c.remaining, c.interval};
        // Linux keeps a CPU timer's interval even when disarming it.
        c.remaining = valueNs; c.interval = intervalNs;
    }
    Reconcile(*g);
    IrqSpinLockRelease(&timerLock, flags);
    if (old) *old = previous;
    return 0;
}
int IntervalTimerGet(Process* p, int which, IntervalTimerValue* out)
{
    if (which < ItimerReal || which > ItimerProf) return -22;
    *out = {};
    uint64_t flags = IrqSpinLockAcquire(&timerLock);
    if (auto* g = ItimersFor(p, false)) {
        if (which == ItimerReal) {
            const auto value = g->real.Read(Now(1));
            *out = {value.remaining, value.interval};
        } else *out = {g->cpu[which - 1].remaining, g->cpu[which - 1].interval};
    }
    IrqSpinLockRelease(&timerLock, flags);
    return 0;
}
void IntervalTimerAlarmConsumed(Process* p)
{
    // Rare (kill-delivered SIGALRM). An expired periodic timer is neither
    // armed nor pending, so no counter can gate this.
    uint64_t flags = IrqSpinLockAcquire(&timerLock);
    if (auto* g = ItimersFor(p, false)) {
        g->real.SharedAlarmDequeued(Now(1));
        Reconcile(*g);
    }
    IrqSpinLockRelease(&timerLock, flags);
}
void IntervalTimerChargeTick(Process* p, bool user, uint64_t ns)
{
    if (!p || p->isKernelThread) return;
    uint64_t flags = IrqSpinLockAcquire(&timerLock);
    if (auto* g = ItimersFor(p, false)) {
        for (int k = user ? 0 : 1; k < 2; ++k) {
            auto& c = g->cpu[k];
            if (!c.remaining) continue;
            if (c.remaining > ns) { c.remaining -= ns; continue; }
            // Linux reloads relative to the expiry, not the observation.
            const uint64_t overshoot = ns - c.remaining;
            c.remaining = c.interval ? (c.interval > overshoot ? c.interval - overshoot : 1) : 0;
            c.pending = true; // coalesces with an undelivered expiry
        }
        const uint8_t before = g->countedPending;
        Reconcile(*g);
        if (g->countedPending & ~before) __atomic_store_n(&itimerUnnotified, 1U, __ATOMIC_RELEASE);
    }
    IrqSpinLockRelease(&timerLock, flags);
}
}
