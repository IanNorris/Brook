#include "watchdog.h"
#include "process.h"
#include "scheduler.h"
#include "smp.h"
#include "serial.h"
#include "panic.h"

namespace brook {

static constexpr uint32_t WATCHDOG_INTERVAL_MS = 1000;
static constexpr uint32_t WATCHDOG_TIMEOUT_MS  = 3000;
static constexpr uint32_t WATCHDOG_STRIKES     = WATCHDOG_TIMEOUT_MS / WATCHDOG_INTERVAL_MS;
static constexpr uint32_t WATCHDOG_MAX_CPUS    = 64;

// Per-CPU snapshot of (busyTicks + idleTicks) from last check.
static uint64_t g_lastTotal[WATCHDOG_MAX_CPUS] = {};
// How many consecutive checks each CPU has been stuck.
static uint32_t g_stuckCount[WATCHDOG_MAX_CPUS] = {};

static void WatchdogThreadFn(void* /*arg*/)
{
    uint32_t cpuCount = SmpGetCpuCount();
    if (cpuCount > WATCHDOG_MAX_CPUS) cpuCount = WATCHDOG_MAX_CPUS;

    // Initialize baselines.
    for (uint32_t c = 0; c < cpuCount; c++)
    {
        uint64_t busy = 0, idle = 0;
        SchedulerGetCpuTicks(c, busy, idle);
        g_lastTotal[c] = busy + idle;
        g_stuckCount[c] = 0;
    }

    for (;;)
    {
        SchedulerSleepMs(WATCHDOG_INTERVAL_MS);

        uint32_t stuckCpus = 0;

        for (uint32_t c = 0; c < cpuCount; c++)
        {
            uint64_t busy = 0, idle = 0;
            SchedulerGetCpuTicks(c, busy, idle);
            uint64_t total = busy + idle;

            if (total == g_lastTotal[c])
            {
                g_stuckCount[c]++;
            }
            else
            {
                g_stuckCount[c] = 0;
                g_lastTotal[c] = total;
            }

            if (g_stuckCount[c] >= WATCHDOG_STRIKES)
                stuckCpus++;
        }

        if (stuckCpus >= cpuCount)
        {
            // ALL CPUs have stopped scheduling — deadlock detected.
            // Dump per-CPU state to serial before panicking.
            SerialPrintf("\n*** WATCHDOG: all %u CPUs stuck for >%ums ***\n",
                         cpuCount, WATCHDOG_TIMEOUT_MS);

            // Dump running processes per CPU.
            for (uint32_t c = 0; c < cpuCount; c++)
            {
                Process* proc = SchedulerGetCpuProcess(c);
                if (proc)
                {
                    SerialPrintf("  CPU%u: pid=%u '%s' state=%u\n",
                                 c, proc->pid, proc->name,
                                 static_cast<uint32_t>(proc->state));
                }
                else
                {
                    SerialPrintf("  CPU%u: <no process>\n", c);
                }
            }

            KernelPanic("WATCHDOG: system deadlock — all %u CPUs"
                        " unresponsive for %u ms", cpuCount, WATCHDOG_TIMEOUT_MS);
        }
    }
}

void WatchdogInit()
{
    KernelThreadCreate("watchdog", WatchdogThreadFn, nullptr, 0 /* highest prio */);
    SerialPrintf("WATCHDOG: started (timeout=%ums, interval=%ums)\n",
                 WATCHDOG_TIMEOUT_MS, WATCHDOG_INTERVAL_MS);
}

} // namespace brook
