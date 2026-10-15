#pragma once
#include <stdint.h>
#include <stddef.h>

namespace brook {
struct Process;
struct SignalInfo;
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
// Caller holds scheduler process-list lock. Never acquires scheduler locks.
void PosixTimersTickLocked(Process* const* processes, uint32_t count);
// exec=true removes the old group's timers at exec commitment. Otherwise
// removes owned timers for leaders with no live siblings, and invalidates a
// departing target TID. Group exit passes true after publishing its death latch.
void PosixTimersCleanup(Process*, bool exec);
}
