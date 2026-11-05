#include "watchdog.h"
#include "watchdog_progress.h"
#include "process.h"
#include "scheduler.h"
#include "smp.h"
#include "serial.h"
#if defined(BROOK_SSD_AUTO_RECOVERY)
#include "panic.h"
#include "panic_test_reset.h"
#endif

namespace brook {

static constexpr uint32_t WATCHDOG_INTERVAL_MS = 1000;
static constexpr uint32_t WATCHDOG_TIMEOUT_MS  = 3000;
static constexpr uint32_t WATCHDOG_STRIKES     = WATCHDOG_TIMEOUT_MS / WATCHDOG_INTERVAL_MS;
static constexpr uint32_t WATCHDOG_MAX_CPUS    = 64;

static WatchdogProgress g_progress[WATCHDOG_MAX_CPUS] = {};
#if defined(BROOK_SSD_AUTO_RECOVERY)
// The software monitor can detect a stalled peer only while this CPU and the
// scheduler still run. Wait substantially longer than the diagnostic warning,
// and escalate only after recovery has durably authorized panic reset.
static constexpr uint32_t RECOVERY_WATCHDOG_STRIKES = 30;
static WatchdogProgress g_recoveryProgress[WATCHDOG_MAX_CPUS] = {};
#endif
static void WatchdogWarning(uint32_t cpu) {
    // A stalled peer may own the ordinary serial ticket lock. Emergency
    // output is bounded even when the UART itself no longer responds.
    SerialPanicPuts("WATCHDOG: CPU");
    if(cpu>=10)SerialPanicPutChar(char('0'+cpu/10));
    SerialPanicPutChar(char('0'+cpu%10));
    SerialPanicPuts(" timer accounting unchanged for 3 checks\n");
}

static void WatchdogThreadFn(void* /*arg*/)
{
    SerialPrintf("WATCHDOG: monitor thread running\n");
    uint32_t cpuCount = SmpGetCpuCount();
    if (cpuCount > WATCHDOG_MAX_CPUS) cpuCount = WATCHDOG_MAX_CPUS;

    // Initialize baselines.
    for (uint32_t c = 0; c < cpuCount; c++)
    {
        uint64_t busy = 0, idle = 0;
        SchedulerGetCpuTicks(c, busy, idle);
        g_progress[c].Reset(busy + idle);
#if defined(BROOK_SSD_AUTO_RECOVERY)
        g_recoveryProgress[c].Reset(busy + idle);
#endif
    }

    for (;;)
    {
        SchedulerSleepMs(WATCHDOG_INTERVAL_MS);

        for (uint32_t c = 0; c < cpuCount; c++)
        {
            uint64_t busy = 0, idle = 0;
            SchedulerGetCpuTicks(c, busy, idle);
            uint64_t total = busy + idle;

            if (g_progress[c].Observe(total, WATCHDOG_STRIKES))
            {
                WatchdogWarning(c);
            }
#if defined(BROOK_SSD_AUTO_RECOVERY)
            // Do not carry a pre-authorization stall across the repair gate.
            // An unseen or already-stalled CPU cannot authorize a reset.
            if (RecoveryWatchdogObserve(g_recoveryProgress[c],PanicRecoveryResetArmed(),total,RECOVERY_WATCHDOG_STRIKES))
                KernelPanic("Recovery watchdog: CPU%u timer accounting stalled for %u checks",c,RECOVERY_WATCHDOG_STRIKES);
#endif
        }
    }
}

void WatchdogInit()
{
    Process* thread = KernelThreadCreate("watchdog", WatchdogThreadFn, nullptr, 0 /* highest prio */);
    if (!thread)
    {
        SerialPrintf("WATCHDOG: failed to create monitor thread\n");
        return;
    }
    SchedulerAddProcess(thread);
    SerialPrintf("WATCHDOG: started (timeout=%ums, interval=%ums)\n",
                 WATCHDOG_TIMEOUT_MS, WATCHDOG_INTERVAL_MS);
}

} // namespace brook
