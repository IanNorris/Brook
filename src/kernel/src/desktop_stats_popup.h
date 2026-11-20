#pragma once
#include <stdint.h>

namespace brook {

enum class DesktopStatsPanel : uint8_t { None, Cpu, Memory, Compositor, App };
inline constexpr unsigned DetailedCoreLimit = 64, DetailedTopTasks = 4;
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
    uint8_t cores[DetailedCoreLimit];
    uint32_t coreCount;
    bool cpuMeasured, tasksMeasured, memoryMeasured, gpuMeasured;
    DesktopTopTask tasks[DetailedTopTasks];
    uint64_t total, used, available, cached, kernel, gpu;
};

void DesktopStatsWorkerStart();
void DesktopStatsSetVisible(DesktopStatsPanel panel);
uint32_t DesktopStatsCoreCount();
void DesktopStatsRead(DesktopStatsPanel panel, DesktopDetailedStats& out);

} // namespace brook
