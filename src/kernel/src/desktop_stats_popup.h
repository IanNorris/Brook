#pragma once
#include <stdint.h>

namespace brook {

enum class DesktopStatsPanel : uint8_t { None, CpuMemory, Compositor, App };
inline constexpr unsigned DetailedCoreLimit = 64, DetailedTopTasks = 4, CompactTopTasks = 3;
inline constexpr uint8_t DetailedNoPercent = 255;

struct DesktopCpuCounter { uint64_t busy, idle; };

inline uint64_t DesktopCpuDelta(DesktopCpuCounter previous, DesktopCpuCounter current, uint8_t& percent) {
    percent = DetailedNoPercent;
    if (current.busy < previous.busy || current.idle < previous.idle) return 0;
    const uint64_t busy = current.busy - previous.busy;
    const uint64_t elapsed = busy + current.idle - previous.idle;
    if (elapsed) percent = uint8_t(100 * busy / elapsed);
    return elapsed;
}

struct DesktopTaskCounter { uint64_t incarnation, ticks; };

inline uint64_t DesktopTaskDelta(DesktopTaskCounter previous, DesktopTaskCounter current) {
    return previous.incarnation && previous.incarnation == current.incarnation && current.ticks >= previous.ticks
        ? current.ticks - previous.ticks : 0;
}

struct DesktopTopTask {
    char name[32];
    uint64_t ticks;
    uint16_t tid;
    uint8_t percent;
};

inline void DesktopRankTask(DesktopTopTask (&top)[DetailedTopTasks], const DesktopTopTask& candidate) {
    if (!candidate.ticks) return;
    for (unsigned i = 0; i < DetailedTopTasks; ++i) {
        if (top[i].ticks > candidate.ticks || (top[i].ticks == candidate.ticks && top[i].tid <= candidate.tid))
            continue;
        for (unsigned j = DetailedTopTasks - 1; j > i; --j) top[j] = top[j - 1];
        top[i] = candidate;
        break;
    }
}

struct DesktopDetailedStats {
    uint8_t cores[DetailedCoreLimit], cpuPercent;
    uint32_t coreCount;
    bool cpuMeasured, tasksMeasured, memoryMeasured, gpuMeasured;
    DesktopTopTask tasks[DetailedTopTasks];
    uint64_t total, used, available, cached, kernel, gpu;
};

inline uint8_t DesktopTaskTotalCpuPercent(uint8_t oneCpuPercent, uint32_t onlineCores) {
    return onlineCores && oneCpuPercent <= 100 ? uint8_t(oneCpuPercent / onlineCores) : DetailedNoPercent;
}

inline uint32_t DesktopStatsTaskRows(const DesktopDetailedStats& stats) {
    uint32_t count = 0;
    if (stats.tasksMeasured) {
        while (count < DetailedTopTasks && stats.tasks[count].ticks) ++count;
    }
    return count ? count : 1;
}

inline int DesktopStatsScrollOffset(int64_t offset, int limit) {
    if (offset <= 0 || limit <= 0) return 0;
    return offset > limit ? limit : int(offset);
}

struct DesktopMemoryRegions { uint64_t other, cache, kernel, available; };

inline DesktopMemoryRegions DesktopMemoryUsageRegions(const DesktopDetailedStats& stats) {
    const uint64_t used = stats.used < stats.total ? stats.used : stats.total;
    const uint64_t free = stats.total - used;
    uint64_t available = stats.available < stats.total ? stats.available : stats.total;
    if (available < free) available = free;

    // MemAvailable includes reclaimable cache. Give that space to the
    // available segment only, so the physical RAM segments add up once.
    const uint64_t reclaimable = available - free;
    const uint64_t occupied = stats.total - available;
    uint64_t cache = stats.cached > reclaimable ? stats.cached - reclaimable : 0;
    if (cache > occupied) cache = occupied;
    const uint64_t remaining = occupied - cache;
    const uint64_t kernel = stats.kernel < remaining ? stats.kernel : remaining;
    return {remaining - kernel, cache, kernel, available};
}

void DesktopStatsWorkerStart();
void DesktopStatsSetVisible(DesktopStatsPanel panel);
uint32_t DesktopStatsCoreCount();
void DesktopStatsRead(DesktopStatsPanel panel, DesktopDetailedStats& out);

} // namespace brook
