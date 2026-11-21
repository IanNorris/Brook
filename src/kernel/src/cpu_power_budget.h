#pragma once
// Closed-loop CPU power budget: caps the IA32_PERF_CTL target ratio so the
// cores (RAPL PP0) stay under a wattage and/or the package under a
// temperature. Off by default; it never touches PERF_CTL while off.
//
// CpuBudgetDecide is the pure control law (one call per 250 ms sampler
// period). CpuPowerBudget wraps it in the same per-CPU snapshot / apply /
// readback / restore discipline as CpuEpbTrial: every CPU snapshots its
// firmware request before any write, each CPU writes only its own MSR (via
// the Read/Write lambdas), a value changed by someone else supersedes the
// budget, and originals are restored with bounded attempts.
#include <stdint.h>

namespace brook {

struct CpuBudgetConfig {
    uint32_t coresMw = 0; // RAPL PP0 target, 0 = no power target
    int32_t tempC = 0;    // package DTS target, 0 = no temperature target
    bool Any() const { return coresMw || tempC; }
};

struct CpuBudgetSample {
    bool powerValid = false, tempValid = false;
    uint32_t coresMw = 0;
    int32_t pkgC = 0;
};

struct CpuBudgetLimits {
    unsigned minRatio = 0;     // MSR_PLATFORM_INFO[47:40]
    unsigned capTop = 0;       // highest cap: min(max non-turbo - 1, baseline - 1)
    unsigned releaseRatio = 0; // highest firmware request (what release returns to)
};

// cap == 0 means released: every CPU runs its firmware request.
struct CpuBudgetFilter {
    bool seeded = false;
    unsigned settle = 0;  // samples still to skip after a step
    unsigned samples = 0; // filtered samples since the last step
    uint32_t mw = 0;      // EWMA of PP0 power, alpha 1/4 (~1 s)
    int32_t temp16 = 0;   // EWMA of package temperature, x16
};

enum class CpuBudgetReason : uint8_t {
    None, NoSample, Settling, Wait, Band, OverPower, OverTemp, HotEmergency, StepUp, Release, AtFloor
};

struct CpuBudgetLaw {
    static constexpr unsigned SettleSamples = 1;   // the sample spanning a step is mixed
    static constexpr unsigned PowerDownSamples = 2; // >= 0.75 s after a step
    static constexpr unsigned TempDownSamples = 8;  // temperature lags power by seconds
    static constexpr unsigned UpSamples = 8;        // >= 2.25 s after a step
    static constexpr int32_t TempBandC = 4;         // raise only this far under the target
    static constexpr int32_t EmergencyC = 5;        // instant reading this far over: step now
    static constexpr uint32_t MinBandMw = 200;
    static uint32_t BandMw(uint32_t target) { return target / 8 > MinBandMw ? target / 8 : MinBandMw; }
};

// One controller step. Steps `cap` by at most one bin (released <-> capTop
// counts as one bin, because requests >= max non-turbo still allow turbo).
inline CpuBudgetReason CpuBudgetDecide(const CpuBudgetConfig& cfg, const CpuBudgetLimits& lim,
                                       CpuBudgetFilter& f, unsigned& cap, const CpuBudgetSample& s)
{
    using L = CpuBudgetLaw;
    if (!cfg.Any() || (cfg.coresMw && !s.powerValid) || (cfg.tempC && !s.tempValid)) return CpuBudgetReason::NoSample;
    if (cap && (cap < lim.minRatio || cap > lim.capTop)) cap = cap < lim.minRatio ? lim.minRatio : lim.capTop;
    if (f.settle) { --f.settle; f.seeded = false; return CpuBudgetReason::Settling; }
    const int32_t temp16 = s.tempValid ? s.pkgC * 16 : 0;
    if (!f.seeded) { f = {}; f.seeded = true; f.mw = s.coresMw; f.temp16 = temp16; f.samples = 1; }
    else {
        f.mw = uint32_t(int64_t(f.mw) + (int64_t(s.coresMw) - int64_t(f.mw)) / 4);
        f.temp16 += (temp16 - f.temp16) / 4;
        ++f.samples;
    }
    const int32_t temp = (f.temp16 + 8) / 16;
    const bool emergency = cfg.tempC && s.pkgC >= cfg.tempC + L::EmergencyC;
    const bool overPower = cfg.coresMw && f.mw > cfg.coresMw && f.samples >= L::PowerDownSamples;
    const bool overTemp = cfg.tempC && temp >= cfg.tempC && f.samples >= L::TempDownSamples;
    auto stepped = [&](unsigned next, CpuBudgetReason why) {
        cap = next;
        f.seeded = false;
        f.settle = L::SettleSamples;
        return why;
    };
    if (emergency || overPower || overTemp) {
        if (cap && cap <= lim.minRatio) return CpuBudgetReason::AtFloor;
        const auto why = emergency ? CpuBudgetReason::HotEmergency
                       : overPower ? CpuBudgetReason::OverPower : CpuBudgetReason::OverTemp;
        return stepped(cap ? cap - 1 : lim.capTop, why);
    }
    if (!cap) return CpuBudgetReason::Band;
    if (f.samples < L::UpSamples) return CpuBudgetReason::Wait;
    const unsigned next = cap >= lim.capTop ? 0 : cap + 1;
    bool powerOk = true;
    if (cfg.coresMw) {
        // Dynamic power scales roughly with f*V^2; f^2 is a conservative
        // lower bound over this range. Predict the next bin before raising.
        const uint64_t to = next ? next : (lim.releaseRatio > lim.capTop ? lim.releaseRatio : lim.capTop + 1);
        const uint64_t predicted = uint64_t(f.mw) * to * to / (uint64_t(cap) * cap);
        powerOk = f.mw + L::BandMw(cfg.coresMw) <= cfg.coresMw && predicted <= cfg.coresMw;
    }
    const bool tempOk = !cfg.tempC || temp <= cfg.tempC - L::TempBandC;
    if (!powerOk || !tempOk) return CpuBudgetReason::Band;
    return stepped(next, next ? CpuBudgetReason::StepUp : CpuBudgetReason::Release);
}

// All calls are serialized by the caller. Tick must run on the CPU whose
// PERF_CTL it accesses; Control may run anywhere (it only moves `cap`).
class CpuPowerBudget {
public:
    static constexpr unsigned MaxCpus = 8;
    static constexpr uint64_t SetupMs = 3000;
    static constexpr unsigned RestoreAttempts = 3;
    static constexpr uint32_t MinMw = 1000, MaxMw = 15000;
    static constexpr int32_t MinTempC = 50, MaxTempC = 95;
    enum class Phase : uint8_t { Off, Snapshot, Active, Restore };
    enum class Outcome : uint8_t { None, Running, Disabled, Ineligible, Timeout, Readback, Superseded, NoRange };

    static bool Valid(const CpuBudgetConfig& c)
    {
        return c.Any() && (!c.coresMw || (c.coresMw >= MinMw && c.coresMw <= MaxMw)) &&
               (!c.tempC || (c.tempC >= MinTempC && c.tempC <= MaxTempC));
    }

    bool Start(uint64_t now, unsigned cpus, const CpuBudgetConfig& c, unsigned minRatio, unsigned maxNonTurbo)
    {
        if (phase != Phase::Off || !cpus || cpus > MaxCpus || !Valid(c) || !minRatio ||
            maxNonTurbo <= minRatio || maxNonTurbo > 255) return false;
        *this = {};
        cpuCount = cpus;
        expected = (1u << cpus) - 1;
        config = c;
        limits.minRatio = minRatio;
        maxNonTurboRatio = maxNonTurbo;
        phase = Phase::Snapshot;
        outcome = Outcome::Running;
        startedAt = now;
        deadline = now + SetupMs;
        return true;
    }

    // Runtime retarget without re-snapshotting.
    bool SetConfig(const CpuBudgetConfig& c)
    {
        if ((phase != Phase::Snapshot && phase != Phase::Active) || !Valid(c)) return false;
        config = c;
        return true;
    }

    void Cancel(Outcome reason = Outcome::Disabled)
    {
        if (phase == Phase::Off || phase == Phase::Restore) return;
        outcome = reason;
        deadline = 0;
        cap = 0;
        phase = touched ? Phase::Restore : Phase::Off;
    }

    void Control(uint64_t now, const CpuBudgetSample& s)
    {
        if (phase != Phase::Active) return;
        const unsigned before = cap;
        lastSample = s;
        lastControlAt = now;
        ++controls;
        lastReason = CpuBudgetDecide(config, limits, filter, cap, s);
        if (cap == before) return;
        lastStepAt = now;
        if (!cap) ++releases;
        else if (!before || cap < before) ++stepsDown;
        else ++stepsUp;
    }

    unsigned Desired(unsigned cpu) const { return cap && cap < original[cpu] ? cap : original[cpu]; }

    template <class Read, class Write>
    void Tick(unsigned cpu, uint64_t now, bool eligible, Read read, Write write)
    {
        if (phase == Phase::Off || cpu >= cpuCount) return;
        if (!eligible) Cancel(Outcome::Ineligible);
        if (phase == Phase::Snapshot && now >= deadline) Cancel(Outcome::Timeout);

        const unsigned bit = 1u << cpu;
        if (phase == Phase::Snapshot) {
            if (!(sampled & bit)) {
                observed[cpu] = original[cpu] = written[cpu] = uint8_t(read());
                sampled |= bit;
            }
            if (sampled == expected) {
                unsigned baseline = 0;
                for (unsigned i = 0; i < cpuCount; ++i) if (original[i] > baseline) baseline = original[i];
                limits.releaseRatio = baseline;
                limits.capTop = maxNonTurboRatio - 1;
                if (baseline && baseline - 1 < limits.capTop) limits.capTop = baseline - 1;
                if (limits.capTop < limits.minRatio) { Cancel(Outcome::NoRange); return; }
                phase = Phase::Active;
                deadline = 0;
            }
        } else if (phase == Phase::Active) {
            const unsigned current = read();
            observed[cpu] = uint8_t(current);
            if (current != written[cpu]) { Cancel(Outcome::Superseded); return; }
            const unsigned want = Desired(cpu);
            if (want == current) return;
            touched |= bit;
            ++writes;
            const unsigned got = write(want);
            // Whatever the register holds after our write is ours to restore.
            observed[cpu] = written[cpu] = uint8_t(got);
            if (got != want) Cancel(Outcome::Readback);
        } else if (phase == Phase::Restore && (touched & bit) && !(restored & bit)) {
            const unsigned current = read();
            observed[cpu] = uint8_t(current);
            // Never overwrite a value someone else wrote after us.
            if (current == written[cpu] && current != original[cpu]) {
                observed[cpu] = written[cpu] = uint8_t(write(original[cpu]));
                ++writes;
            }
            if (observed[cpu] == original[cpu]) {
                restored |= bit;
            } else if (observed[cpu] != written[cpu] || ++restoreAttempts[cpu] == RestoreAttempts) {
                restoreFailed |= bit;
                restored |= bit;
            }
            if ((restored & touched) == touched) phase = Phase::Off;
        }
    }

    Phase phase = Phase::Off;
    Outcome outcome = Outcome::None;
    CpuBudgetConfig config{};
    CpuBudgetLimits limits{};
    CpuBudgetFilter filter{};
    CpuBudgetSample lastSample{};
    CpuBudgetReason lastReason = CpuBudgetReason::None;
    unsigned cap = 0, cpuCount = 0, maxNonTurboRatio = 0;
    unsigned sampled = 0, touched = 0, restored = 0, restoreFailed = 0;
    uint64_t deadline = 0, startedAt = 0, lastControlAt = 0, lastStepAt = 0;
    uint64_t controls = 0, stepsDown = 0, stepsUp = 0, releases = 0, writes = 0;
    uint8_t original[MaxCpus]{}, observed[MaxCpus]{}, written[MaxCpus]{};

private:
    unsigned expected = 0;
    uint8_t restoreAttempts[MaxCpus]{};
};

} // namespace brook
