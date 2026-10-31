#include "watchdog.h"
#include "watchdog_progress.h"
#include "process.h"
#include "scheduler.h"
#include "smp.h"
#include "serial.h"

namespace brook {

static constexpr uint32_t WATCHDOG_INTERVAL_MS = 1000;
static constexpr uint32_t WATCHDOG_TIMEOUT_MS  = 3000;
static constexpr uint32_t WATCHDOG_STRIKES     = WATCHDOG_TIMEOUT_MS / WATCHDOG_INTERVAL_MS;
static constexpr uint32_t WATCHDOG_MAX_CPUS    = 64;

static WatchdogProgress g_progress[WATCHDOG_MAX_CPUS] = {};

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
                SerialPrintf("WATCHDOG: CPU%u timer accounting unchanged for %u checks\n",
                             c, WATCHDOG_STRIKES);
            }
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
