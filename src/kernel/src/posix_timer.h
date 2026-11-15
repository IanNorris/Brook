#pragma once
#include <stdint.h>
#include <stddef.h>

namespace brook {
struct Process;
struct SignalInfo;
struct KernelSigaction;
// Linux x86-64 syscall ABI, independent of libc's opaque timer_t.
struct PosixSigevent {
    uint64_t value;
    int32_t signo;
    int32_t notify;
    int32_t targetTid;
    uint8_t padding[44];
};
struct PosixTimespec { int64_t sec, nsec; };
struct PosixItimerspec { PosixTimespec interval, value; };
static_assert(sizeof(PosixSigevent) == 64);
static_assert(offsetof(PosixSigevent, value) == 0);
static_assert(offsetof(PosixSigevent, signo) == 8);
static_assert(offsetof(PosixSigevent, notify) == 12);
static_assert(offsetof(PosixSigevent, targetTid) == 16);
static_assert(sizeof(PosixItimerspec) == 32);
int PosixTimerCreate(Process*, int clock, const PosixSigevent*, int32_t* id);
int PosixTimerSet(Process*, int32_t id, unsigned flags,
                  const PosixItimerspec&, PosixItimerspec* old);
int PosixTimerGet(Process*, int32_t id, PosixItimerspec*);
int PosixTimerOverrun(Process*, int32_t id);
int PosixTimerDelete(Process*, int32_t id);
uint64_t PosixTimerPending(Process*);
int PosixTimerDequeue(Process*, uint64_t mask, SignalInfo*);
// Caller must select an unblocked signal for the current task.
bool PosixTimerClaimCaught(Process*,int signum,SignalInfo*,KernelSigaction*);
// Caller holds scheduler process-list lock. Never acquires scheduler locks.
void PosixTimersTickLocked(Process* const* processes, uint32_t count);
// exec=true removes the old group's timers at exec commitment. Otherwise
// removes owned timers for leaders with no live siblings, and invalidates a
// departing target TID. Group exit passes true after publishing its death latch.
void PosixTimersCleanup(Process*, bool exec);

// setitimer/getitimer interval timers and alarm(). One record per thread
// group: shared by its threads, not inherited by fork, preserved across exec
// (by the group leader). REAL runs on elapsed time; a periodic REAL timer
// restarts when its SIGALRM is dequeued, as on Linux. VIRTUAL and PROF are
// charged from the 1 ms scheduler tick (user ticks, user+kernel ticks) and
// reload at expiry. Expiry signals are process-directed with SI_KERNEL and
// share the POSIX timer delivery path above.
enum : int { ItimerReal = 0, ItimerVirtual = 1, ItimerProf = 2 };
struct IntervalTimerValue { uint64_t valueNs, intervalNs; };
int IntervalTimerSet(Process*, int which, uint64_t valueNs, uint64_t intervalNs,
                     IntervalTimerValue* old);
int IntervalTimerGet(Process*, int which, IntervalTimerValue*);
// The caller consumed a process-directed SIGALRM from the ordinary signal
// queue (kill). Like Linux's shared-queue dequeue, this restarts an expired
// periodic REAL timer and absorbs its coalesced pending expiry.
void IntervalTimerAlarmConsumed(Process*);
// Nonzero while any VIRTUAL/PROF timer is armed; the scheduler tick tests it
// before charging, so an unarmed system pays one load per tick.
extern uint32_t g_intervalCpuTimersArmed;
inline bool IntervalTimerCpuArmed()
{
    return __atomic_load_n(&g_intervalCpuTimersArmed, __ATOMIC_RELAXED) != 0;
}
void IntervalTimerChargeTick(Process* current, bool user, uint64_t ns);
// After PosixTimersTickLocked, with the registry lock released: interval
// timer expiries whose signal is unblocked with SIG_DFL (terminate). Each
// target carries a process reference; the caller sends the signal through
// ProcessSendSignal and drops the reference.
uint32_t PosixTimersTakeFatal(Process** targets, int* signals, uint32_t capacity);
}
