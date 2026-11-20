#include "desktop_stats_popup.h"
#include "apic.h"
#include "compositor.h"
#include "intel_bdw_scanout.h"
#include "memory/heap.h"
#include "memory/physical_memory.h"
#include "memory/pmm_accounting.h"
#include "process.h"
#include "scheduler.h"
#include "smp.h"
#include "spinlock.h"
#include "string.h"
#include "sync/ksemaphore.h"

namespace brook {
namespace {

static_assert(MAX_CPUS <= DetailedCoreLimit);
struct CpuWorkspace {
    DesktopCpuCounter cores[DetailedCoreLimit];
    DesktopTaskCounter tasks[MAX_PROCESSES];
    DebugTaskSnapshot entries[MAX_PROCESSES];
    uint32_t count;
    bool haveTasks;
};

bool g_started, g_wakeQueued;
uint32_t g_request; // low byte is the panel, upper bits distinguish each opening
KSemaphore g_open;
SpinLock g_lock;
DesktopDetailedStats g_snapshot;
uint32_t g_published;

uint32_t Request() { return __atomic_load_n(&g_request, __ATOMIC_ACQUIRE); }
DesktopStatsPanel Panel(uint32_t request) { return DesktopStatsPanel(uint8_t(request)); }

void SampleCpu(CpuWorkspace& work, DesktopDetailedStats& out)
{
    out.coreCount = DesktopStatsCoreCount();
    uint64_t elapsed = 0, busy = 0;
    uint32_t usable = 0;
    for (uint32_t i = 0; i < out.coreCount; ++i) {
        DesktopCpuCounter current{};
        SchedulerGetCpuTicks(i, current.busy, current.idle);
        out.cores[i] = DetailedNoPercent;
        if (i < work.count) {
            const auto ticks = DesktopCpuDelta(work.cores[i], current, out.cores[i]);
            if (ticks) {
                ++usable;
                elapsed += ticks;
                busy += current.busy - work.cores[i].busy;
            }
        }
        work.cores[i] = current;
    }
    work.count = out.coreCount;
    out.cpuMeasured = elapsed != 0;
    out.cpuPercent = elapsed ? uint8_t(100 * busy / elapsed) : DetailedNoPercent;

    uint32_t total = 0;
    uint64_t begin = 0, end = 0;
    const uint32_t count = SchedulerSnapshotDebugTasks(work.entries, MAX_PROCESSES, &total, &begin, &end);
    const uint64_t oneCpu = usable ? elapsed / usable : 0;
    out.tasksMeasured = work.haveTasks && oneCpu && count == total;
    work.haveTasks = count == total;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& task = work.entries[i];
        if (task.pid >= MAX_PROCESSES || task.state == ProcessState::Terminated) continue;
        const DesktopTaskCounter current{task.incarnation, task.userTicks + task.sysTicks};
        const uint64_t ticks = DesktopTaskDelta(work.tasks[task.pid], current);
        work.tasks[task.pid] = current;
        if (!out.tasksMeasured || !ticks) continue;
        DesktopTopTask row{};
        memcpy(row.name, task.name, sizeof(row.name) - 1);
        row.ticks = ticks;
        row.tid = task.pid;
        row.percent = uint8_t(ticks >= oneCpu ? 100 : 100 * ticks / oneCpu);
        DesktopRankTask(out.tasks, row);
    }
}

void SampleMemory(DesktopDetailedStats& out)
{
    const auto memory = PmmReadOwnerStats();
    if (!memory.ramPages) return;
    constexpr uint64_t page = 4096;
    out.memoryMeasured = true;
    out.total = memory.ramPages * page;
    out.used = (memory.ramPages >= memory.freePages ? memory.ramPages - memory.freePages : 0) * page;
    out.available = memory.AvailablePages() * page;
    out.cached = memory.CachedPages() * page;
    out.gpu = (memory.Pages(PmmOwner::GpuBuffer) + memory.Pages(PmmOwner::Scanout)) * page;
    // The PMM classes track Broadwell system-memory backing, not GPU VRAM
    // reported by an external GPU or by the QEMU host.
    out.gpuMeasured = out.gpu || BdwIntelUserNodeEnabled();
    const PmmOwner kernel[] = {PmmOwner::KernelHeap, PmmOwner::PageTables, PmmOwner::KernelStacks,
        PmmOwner::XhciDma, PmmOwner::AhciDma, PmmOwner::HdaDma, PmmOwner::NetworkDma, PmmOwner::DriverDma};
    for (const auto owner : kernel) out.kernel += memory.Pages(owner) * page;
}

void Worker(void*)
{
    CpuWorkspace* cpu = nullptr;
    uint32_t served = 0;
    uint64_t last = 0;
    for (;;) {
        const uint32_t request = Request();
        const auto panel = Panel(request);
        if (panel == DesktopStatsPanel::None || request != served) {
            if (cpu) kfree(cpu);
            cpu = nullptr;
        }
        if (panel == DesktopStatsPanel::None) {
            KSemaphoreWait(&g_open);
            __atomic_store_n(&g_wakeQueued, false, __ATOMIC_RELEASE);
            continue;
        }
        const auto now = ApicTickCount();
        if (served != request || now - last >= 1000) {
            DesktopDetailedStats sample{};
            if (!cpu) {
                cpu = static_cast<CpuWorkspace*>(kmalloc(sizeof(CpuWorkspace)));
                if (cpu) memset(cpu, 0, sizeof(*cpu));
            }
            if (cpu) SampleCpu(*cpu, sample);
            SampleMemory(sample);
            SpinLockAcquire(&g_lock);
            if (Request() == request) {
                g_snapshot = sample;
                g_published = request;
            }
            SpinLockRelease(&g_lock);
            if (Request() == request) CompositorWake();
            served = request;
            last = now;
        }
        SchedulerSleepMs(250);
    }
}
} // namespace

void DesktopStatsWorkerStart()
{
    if (__atomic_exchange_n(&g_started, true, __ATOMIC_ACQ_REL)) return;
    if (Process* worker = KernelThreadCreate("desktop-stats", Worker, nullptr, 3))
        SchedulerAddProcess(worker);
    else
        __atomic_store_n(&g_started, false, __ATOMIC_RELEASE);
}

void DesktopStatsSetVisible(DesktopStatsPanel panel)
{
    if (panel != DesktopStatsPanel::CpuMemory) panel = DesktopStatsPanel::None;
    const auto previous = Request();
    if (Panel(previous) == panel) return;
    const uint32_t request = ((previous + 256) & ~uint32_t{255}) | uint8_t(panel);
    __atomic_store_n(&g_request, request, __ATOMIC_RELEASE);
    if (panel != DesktopStatsPanel::None &&
        !__atomic_exchange_n(&g_wakeQueued, true, __ATOMIC_ACQ_REL)) KSemaphoreSignal(&g_open);
}

uint32_t DesktopStatsCoreCount()
{
    const auto count = SmpGetCpuCount();
    return count < DetailedCoreLimit ? count : DetailedCoreLimit;
}

void DesktopStatsRead(DesktopStatsPanel panel, DesktopDetailedStats& out)
{
    const uint32_t request = Request();
    if (Panel(request) != panel) return;
    SpinLockAcquire(&g_lock);
    if (g_published == request) out = g_snapshot;
    SpinLockRelease(&g_lock);
}

} // namespace brook
