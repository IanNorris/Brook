#include "process_user_namespace.h"
#include "sysv_sem.h"
#include "process.h"
#include "scheduler.h"
#include "spinlock.h"
#include "memory/heap.h"
#include "rtc.h"
#include "posix_timer.h"
#include "apic.h"
#include "kvmclock.h"
namespace brook {
static bool SemInterrupted(Process* p) {
    return __atomic_load_n(&p->terminalPending,__ATOMIC_ACQUIRE) ||
        ((ProcessPendingSignals(p) | PosixTimerPending(p)) & ~p->sigMask);
}
static uint64_t SemNowNs() {
    return KvmClockAvailable()?KvmClockReadNs():ApicTickCount()*1000000ULL;
}
static uint64_t SemWakeupTick(uint64_t deadline) {
    return SysvSemWakeTick(deadline,SemNowNs(),ApicTickCount());
}
#include "sysv_sem_service.inc"
}
