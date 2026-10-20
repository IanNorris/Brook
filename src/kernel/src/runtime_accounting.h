#pragma once
#include <stdint.h>

namespace brook {
struct RuntimeSample {
    uint64_t wallNs = 0, stealNs = 0;
    bool valid = false;
};
// A host preemption can occur between the two different KVM shared records.
// Bracket the wall read with steal reads; retry the entire pair on change.
// Failure is bounded and callers must discard that unknown interval.
template<class WallReader, class StealReader>
RuntimeSample RuntimeReadSample(bool hasSteal, WallReader wallReader, StealReader stealReader) {
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        uint64_t before = 0, wall = 0, after = 0;
        if (hasSteal && !stealReader(&before)) return {};
        if (!wallReader(&wall)) return {};
        if (hasSteal && !stealReader(&after)) return {};
        if (before == after) return {wall, before, true};
    }
    return {};
}
struct RuntimeClampEvidence {
    uint64_t sample=0,previousWallNs=0,wallNs=0,previousStealNs=0,stealNs=0;
    uint64_t serviceFloorNs=0,deficitNs=0,incarnation=0;
    uint32_t cpu=0;
    uint16_t pid=0;
};
struct RuntimeDiagnostics {
    uint64_t samples = 0, chargedNs = 0, idleNs = 0, invalidSamples = 0;
    uint64_t backwardsWall = 0, backwardsSteal = 0, clampedService = 0;
    uint64_t staleIdentity = 0, saturated = 0, observedStealNs = 0;
    uint64_t maxWallRewindNs = 0, maxStealRewindNs = 0, backwardsCpuMask = 0;
    uint64_t lastBackwardsSample = 0, lastPreviousWallNs = 0, lastWallNs = 0;
    uint64_t lastPreviousStealNs = 0, lastStealNs = 0;
    uint32_t lastBackwardsCpu = 0;
    uint16_t lastBackwardsPid = 0;
    RuntimeClampEvidence firstClamp{},maxClamp{};
};

// Caller serializes all access. Includes kernel/IRQ work while a task owns the
// CPU; excludes blocked intervals and reported host steal. No allocation or
// Process pointers. A failed clock sample breaks the interval rather than
// charging an unobserved gap to the next task. Host steal updates can lag: a
// service floor carries that correction forward without subtracting task time.
template<unsigned Tasks, unsigned Cpus> class RuntimeAccounting {
    struct Task {
        uint64_t incarnation = 0, groupIncarnation = 0;
        uint64_t threadNs = 0, groupNs = 0;
        uint16_t group = 0;
    } tasks_[Tasks]{};
    struct Cpu {
        uint64_t wall = 0, steal = 0, serviceFloor = 0, incarnation = 0;
        uint16_t pid = 0;
        bool baseline = false;
    } cpus_[Cpus]{};
    RuntimeDiagnostics diagnostics_{};

    void Add(uint64_t& value, uint64_t amount) {
        if (amount > ~uint64_t(0) - value) {
            value = ~uint64_t(0); ++diagnostics_.saturated;
        } else value += amount;
    }
public:
    bool Register(uint16_t pid, uint64_t incarnation, uint16_t group, uint64_t groupIncarnation) {
        if (!pid || pid >= Tasks || !incarnation || !group || group >= Tasks || !groupIncarnation)
            return false;
        auto& task = tasks_[pid];
        if (task.incarnation == incarnation) return true; // exec/policy switch cannot reset service
        task = {}; task.incarnation = incarnation;
        task.group = group; task.groupIncarnation = groupIncarnation;
        return true;
    }
    // Returns the service charged at this boundary (for a future policy ABI).
    uint64_t Sample(unsigned cpu, RuntimeSample sample) {
        if (cpu >= Cpus) return 0;
        auto& state = cpus_[cpu]; ++diagnostics_.samples;
        if (!sample.valid || sample.stealNs > sample.wallNs) {
            ++diagnostics_.invalidSamples; state.baseline = false; return 0;
        }
        const uint64_t service = sample.wallNs - sample.stealNs;
        if (!state.baseline) {
            state.wall = sample.wallNs; state.steal = sample.stealNs;
            state.serviceFloor = service; state.baseline = true; return 0;
        }
        if (sample.wallNs < state.wall || sample.stealNs < state.steal) {
            if (sample.wallNs < state.wall) {
                ++diagnostics_.backwardsWall;
                const uint64_t rewind = state.wall - sample.wallNs;
                if (rewind > diagnostics_.maxWallRewindNs) diagnostics_.maxWallRewindNs = rewind;
            }
            if (sample.stealNs < state.steal) {
                ++diagnostics_.backwardsSteal;
                const uint64_t rewind = state.steal - sample.stealNs;
                if (rewind > diagnostics_.maxStealRewindNs) diagnostics_.maxStealRewindNs = rewind;
            }
            if (cpu < 64) diagnostics_.backwardsCpuMask |= uint64_t(1) << cpu;
            diagnostics_.lastBackwardsCpu = cpu;
            diagnostics_.lastBackwardsPid = state.pid;
            diagnostics_.lastBackwardsSample = diagnostics_.samples;
            diagnostics_.lastPreviousWallNs = state.wall;
            diagnostics_.lastWallNs = sample.wallNs;
            diagnostics_.lastPreviousStealNs = state.steal;
            diagnostics_.lastStealNs = sample.stealNs;
            // Drop the unknown interval, including across the next dispatch.
            // Per-task cumulative clocks themselves never regress.
            state.baseline = false; return 0;
        }
        Add(diagnostics_.observedStealNs, sample.stealNs - state.steal);
        const uint64_t previousWall=state.wall,previousSteal=state.steal;
        state.wall = sample.wallNs; state.steal = sample.stealNs;
        if (service < state.serviceFloor) {
            ++diagnostics_.clampedService;
            const RuntimeClampEvidence evidence{diagnostics_.samples,previousWall,sample.wallNs,
                previousSteal,sample.stealNs,state.serviceFloor,state.serviceFloor-service,
                state.incarnation,cpu,state.pid};
            if(diagnostics_.clampedService==1)diagnostics_.firstClamp=evidence;
            if(evidence.deficitNs>diagnostics_.maxClamp.deficitNs)diagnostics_.maxClamp=evidence;
            return 0;
        }
        const uint64_t delta = service - state.serviceFloor;
        state.serviceFloor = service;
        if (!state.pid) { Add(diagnostics_.idleNs, delta); return 0; }
        if (state.pid >= Tasks || tasks_[state.pid].incarnation != state.incarnation) {
            ++diagnostics_.staleIdentity; return 0;
        }
        auto& task = tasks_[state.pid];
        Add(task.threadNs, delta); Add(diagnostics_.chargedNs, delta);
        if (tasks_[task.group].incarnation == task.groupIncarnation)
            Add(tasks_[task.group].groupNs, delta);
        else ++diagnostics_.staleIdentity;
        return delta;
    }
    void Dispatch(unsigned cpu, uint16_t pid, uint64_t incarnation, RuntimeSample sample) {
        if (cpu >= Cpus) return;
        Sample(cpu, sample);
        SetOwner(cpu, pid, incarnation);
    }
    // Kernel integration flushes through the policy callback first, then changes
    // only the ledger owner. Dispatch above remains the combined model API.
    void SetOwner(unsigned cpu, uint16_t pid, uint64_t incarnation) {
        if (cpu >= Cpus) return;
        cpus_[cpu].pid = pid; cpus_[cpu].incarnation = incarnation;
    }
    bool Read(uint16_t pid, uint64_t incarnation, bool group, uint64_t* output) const {
        if (!output || !pid || pid >= Tasks || tasks_[pid].incarnation != incarnation) return false;
        *output = group ? tasks_[pid].groupNs : tasks_[pid].threadNs;
        return true;
    }
    bool Rebind(uint16_t pid, uint64_t incarnation, uint16_t group, uint64_t groupIncarnation) {
        if (!pid || pid >= Tasks || !group || group >= Tasks || !groupIncarnation ||
            tasks_[pid].incarnation != incarnation) return false;
        auto& task = tasks_[pid];
        if (group == pid && task.group != pid) task.groupNs = task.threadNs;
        task.group = group; task.groupIncarnation = groupIncarnation;
        return true;
    }
    const RuntimeDiagnostics& Diagnostics() const { return diagnostics_; }
};
} // namespace brook
