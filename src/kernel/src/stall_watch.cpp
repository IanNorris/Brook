#include "stall_watch.h"
#include "smp.h"
#include "process.h"
#include "scheduler.h"
#include "tsc_clock.h"
#include "apic.h"
#include "kprintf.h"

namespace brook {
namespace {
struct alignas(64) CpuStamp { uint64_t ns; uint64_t rip; uint32_t pid; uint32_t ticks; };
CpuStamp g_stamp[MAX_CPUS];
const char* volatile g_trigger = nullptr;
uint64_t g_lastDumpNs = 0;
uint32_t g_dumps = 0;

uint64_t NowNs() { return TscClockAvailable() ? TscClockReadNs() : ApicTickCount() * 1000000ull; }

void Dump(const char* why, uint64_t late_ms) {
    const uint64_t now = NowNs();
    if (g_lastDumpNs && now - g_lastDumpNs < 5000000000ull) return;
    g_lastDumpNs = now; ++g_dumps;
    KPrintf("stall-watch #%u: %s (watcher late %lu ms) at %lu ms\n", g_dumps, why, (unsigned long)late_ms,
            (unsigned long)(now / 1000000));
    for (uint32_t c = 0; c < MAX_CPUS; ++c) {
        const uint64_t ns = __atomic_load_n(&g_stamp[c].ns, __ATOMIC_ACQUIRE);
        if (!ns) continue;
        // stamps come from other CPUs' clocks: a stamp slightly ahead of `now` is age 0, not huge
        const int64_t age = int64_t(now - ns);
        KPrintf("stall-watch   cpu %u: last tick %ld ms ago, rip %lx, pid %u, ticks %u\n", c,
                (long)(age > 0 ? age / 1000000 : 0), (unsigned long)__atomic_load_n(&g_stamp[c].rip, __ATOMIC_RELAXED),
                __atomic_load_n(&g_stamp[c].pid, __ATOMIC_RELAXED), __atomic_load_n(&g_stamp[c].ticks, __ATOMIC_RELAXED));
    }
}

void Watcher(void*) {
    uint64_t prev = NowNs();
    for (;;) {
        SchedulerSleepMs(50);
        const uint64_t now = NowNs();
        const uint64_t late = now - prev > 50000000ull ? (now - prev - 50000000ull) / 1000000 : 0;
        prev = now;
        const char* t = __atomic_exchange_n(&g_trigger, static_cast<const char*>(nullptr), __ATOMIC_ACQ_REL);
        if (late > 500) Dump("watcher woke late", late);
        else if (t) Dump(t, late);
    }
}
}  // namespace

void StallWatchTick(uint64_t rip) {
    const uint32_t c = SmpCurrentCpuIndex();
    if (c >= MAX_CPUS) return;
    CpuStamp& s = g_stamp[c];
    const Process* p = ProcessCurrent();
    __atomic_store_n(&s.rip, rip, __ATOMIC_RELAXED);
    __atomic_store_n(&s.pid, p ? uint32_t(p->pid) : 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s.ticks, s.ticks + 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s.ns, NowNs(), __ATOMIC_RELEASE);
}

void StallWatchTrigger(const char* why) { __atomic_store_n(&g_trigger, why, __ATOMIC_RELEASE); }

void StallWatchStart() {
    Process* t = KernelThreadCreate("stall_watch", Watcher, nullptr, 0);
    if (t) SchedulerAddProcess(t);
    else KPrintf("stall-watch: thread create failed\n");
}
}  // namespace brook
