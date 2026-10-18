#pragma once
#include <stdint.h>

namespace brook {
// All tracker access is serialized by the scheduler's existing ready lock.
// Storage is bounded, holds no Process pointers, and is disabled at boot.
static constexpr uint64_t ReadyLatencyBoundsNs[] = {
    1000, 5000, 10000, 25000, 50000, 100000, 250000, 500000,
    1000000, 2500000, 5000000, 10000000, 25000000, 50000000, 100000000, 1000000000
};
static constexpr unsigned ReadyLatencyBins = sizeof(ReadyLatencyBoundsNs) / sizeof(uint64_t) + 1;
struct ReadyLatencyCounts {
    uint64_t samples = 0, sumNs = 0, maximumNs = 0;
    uint64_t maximumPid = 0, maximumIncarnation = 0;
    uint64_t bins[ReadyLatencyBins]{};
    bool sumSaturated = false;
};
struct ReadyLatencySnapshot {
    bool enabled = false;
    char policy[32]{};
    uint64_t resetGeneration = 0, enqueues = 0, cancelled = 0, pending = 0;
    uint64_t untrackedDispatches = 0, unavailableClock = 0, backwardsClock = 0;
    uint64_t preexistingQueued = 0, preexistingDispatches = 0;
    ReadyLatencyCounts user{}, kernel{};
};
template<unsigned Slots> class ReadyLatencyTracker {
    struct Pending {
        uint64_t incarnation = 0, sinceNs = 0;
        bool queued = false, kernel = false, preexisting = false;
    } pending_[Slots]{};
    ReadyLatencySnapshot snapshot_{};
public:
    bool Enabled() const { return snapshot_.enabled; }
    void Reset() {
        const auto generation = snapshot_.resetGeneration + 1;
        const bool enabled = snapshot_.enabled;
        snapshot_ = {}; snapshot_.enabled = enabled; snapshot_.resetGeneration = generation;
        for (auto& pending : pending_) pending = {};
    }
    void Enable(bool enabled) {
        if (enabled && !snapshot_.enabled) Reset();
        snapshot_.enabled = enabled;
    }
    void Enqueue(unsigned pid, uint64_t incarnation, bool kernel, uint64_t nowNs) {
        if (!Enabled() || !pid || pid >= Slots) return;
        auto& pending = pending_[pid];
        if (pending.incarnation != incarnation) {
            if (pending.queued) { --snapshot_.pending; ++snapshot_.cancelled; }
            pending = {}; pending.incarnation = incarnation;
        }
        // Affinity retries and idempotent re-enqueue preserve the first time.
        if (pending.queued) return;
        pending.sinceNs = nowNs; pending.queued = true; pending.kernel = kernel;
        pending.preexisting = false;
        ++snapshot_.enqueues; ++snapshot_.pending;
        if (!nowNs) ++snapshot_.unavailableClock;
    }
    // Entries already queued when enabling/resetting have no start timestamp.
    // Keep retries from turning them into misleading partial latency samples.
    void SeedPending(unsigned pid, uint64_t incarnation, bool kernel) {
        if (!Enabled() || !pid || pid >= Slots) return;
        auto& pending = pending_[pid];
        if (pending.queued && pending.incarnation == incarnation) return;
        if (pending.queued) { --snapshot_.pending; ++snapshot_.cancelled; }
        pending = {incarnation, 0, true, kernel, true};
        ++snapshot_.pending; ++snapshot_.preexistingQueued;
    }
    void Remove(unsigned pid, uint64_t incarnation) {
        if (!Enabled() || !pid || pid >= Slots) return;
        auto& pending = pending_[pid];
        if (pending.incarnation != incarnation || !pending.queued) return;
        pending.queued = false; --snapshot_.pending; ++snapshot_.cancelled;
    }
    void Dispatch(unsigned pid, uint64_t incarnation, uint64_t nowNs) {
        if (!Enabled() || !pid || pid >= Slots) return;
        auto& pending = pending_[pid];
        if (pending.incarnation != incarnation || !pending.queued) {
            ++snapshot_.untrackedDispatches; return;
        }
        pending.queued = false; --snapshot_.pending;
        if (pending.preexisting) { ++snapshot_.preexistingDispatches; return; }
        if (!pending.sinceNs || !nowNs) {
            if (!nowNs) ++snapshot_.unavailableClock;
            return;
        }
        if (nowNs < pending.sinceNs) { ++snapshot_.backwardsClock; return; }
        const auto elapsed = nowNs - pending.sinceNs;
        auto& counts = pending.kernel ? snapshot_.kernel : snapshot_.user;
        ++counts.samples;
        if (elapsed > ~uint64_t{0} - counts.sumNs) {
            counts.sumNs = ~uint64_t{0}; counts.sumSaturated = true;
        } else counts.sumNs += elapsed;
        if (counts.samples == 1 || elapsed > counts.maximumNs) {
            counts.maximumNs = elapsed;
            counts.maximumPid = pid; counts.maximumIncarnation = incarnation;
        }
        unsigned bin = 0;
        while (bin + 1 < ReadyLatencyBins && elapsed > ReadyLatencyBoundsNs[bin]) ++bin;
        ++counts.bins[bin];
    }
    ReadyLatencySnapshot Snapshot() const { return snapshot_; }
};
enum class ReadyLatencyAction { Status, On, Off, Reset };
} // namespace brook
