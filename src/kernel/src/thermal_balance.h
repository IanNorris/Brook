#pragma once
// Combined CPU + GPU thermal balance controller (opt-in, `thermal balance`).
//
// Under sustained heavy load it caps the CPU slightly below base (max
// non-turbo - 1) and spends the freed package headroom on a higher render
// clock, keeping the filtered package temperature under a target well below
// the 90 C GPU trip. Light load releases the CPU to its firmware request
// (turbo) and the GPU to its default request.
//
// This header is the pure control law, one Decide call per 250 ms thermal
// sampler period. It only moves two numbers: `cap` (PERF_CTL ratio cap, 0 =
// released) and `gpuMhz` (render clock request). The kernel applies them via
// the CpuPowerBudget PERF_CTL owner (external mode) and the render clock
// balance overlay; all MSR / GPU access stays behind those owners.
//
// Feedback: package DTS, filtered (fast EWMA ~0.75 s, slow ~3.75 s); the
// fast-slow lag difference gives a slope and a 4 s prediction (lead), so the
// law steps down before the target instead of after (temperature-only control
// without lead was too slow on the Dell: 7 turbo releases, 91 C, a trip).
// Package power (not PP0: PP0 rises with GT-driven ring/LLC load, which made
// the PP0 budget starve the CPU when the GPU clock went up) is a sanity limit
// and a GPU step feed-forward. No absolute temperature besides the target is
// built in, so a re-pasted laptop simply settles at a different operating point.
//
// Ladder (one knob, one bin per decision; a spike may move two):
//   lower: CPU cap down to the floor -> GPU down to base -> CPU below floor (to lowestRatio)
//   raise: CPU up to the floor -> GPU up to RP0 (only while GPU-bound) -> CPU up to capTop
// so GPU boost is always given up before the CPU goes below its floor, and in a
// GPU-bound load the cheap CPU bins above the floor are traded first.
#include <stdint.h>

namespace brook {

struct BalanceConfig {
    int32_t targetC = 75;       // filtered package temperature target
    uint32_t pkgLimitMw = 13000; // filtered package power sanity limit (firmware PL1 is 15 W)
};

struct BalanceLimits {
    unsigned minRatio = 0;   // MSR_PLATFORM_INFO[47:40]
    unsigned floorRatio = 0; // normal lowest CPU cap (capTop - FloorBins)
    // Absolute lowest cap (2/3 of the floor, 13 on the Dell), used only once
    // the GPU boost is gone. Lower still, Chromium/dav1d turn CPU-bound while
    // PP0 hardly falls (its large frequency-independent part), so it costs
    // fps without cooling; the 90 C trip and PROCHOT remain the backstops.
    unsigned lowestRatio = 0;
    unsigned capTop = 0;     // max non-turbo - 1 (from the PERF_CTL owner)
    uint32_t gpuBaseMhz = 600; // default request (fixed boot value)
    uint32_t gpuMaxMhz = 600;  // fused RP0; == base disables GPU boost
};

struct BalanceSample {
    bool tempValid = false, powerValid = false, loadValid = false;
    int32_t pkgC = 0;            // instantaneous package DTS
    uint32_t pkgMw = 0, gtMw = 0; // RAPL package and PP1 (GT) over the last period
    uint32_t c0Pct = 0;          // average C0 residency over CPUs
    uint32_t gpuBusyPermille = 0; // render worker application+compositor occupancy
    bool gpuTripped = false;     // the 90 C render-clock fallback is active
};

enum class BalanceMode : uint8_t { Light, Heavy };
enum class BalanceReason : uint8_t {
    None, NoSample, Settling, Wait, Band, LoadHeavy, LoadLight, HoldHot, Spike, Trip,
    OverTemp, Predicted, OverPower, GpuIdle, Headroom
};
enum class BalanceAction : uint8_t {
    None, Engage, Release, CpuDown, CpuUp, GpuDown, GpuUp, CpuCapHot, AtFloor, AtTop
};

struct BalanceLaw {
    static constexpr unsigned EnterSamples = 4;          // 1 s of heavy load before engaging
    static constexpr unsigned LightSamples = 12;         // 3 s of light load before releasing
    static constexpr unsigned MaxLightSamples = 120;     // release hold backs off to 30 s
    static constexpr uint64_t ReengageMs = 30000;        // re-engage this soon after a release: back off
    static constexpr uint64_t CalmMs = 60000;            // this long without either backoff event: halve
    static constexpr uint32_t BusyHeavyPermille = 800, BusyLightPermille = 500;
    static constexpr uint32_t C0HeavyPct = 70, C0LightPct = 40;
    static constexpr unsigned SettleSamples = 1;         // the sample spanning a step is mixed
    static constexpr unsigned DownSamples = 2;           // >= 0.5 s between down steps
    static constexpr unsigned PowerDownSamples = 4;      // power filter is slower
    static constexpr unsigned UpSamples = 12;            // >= 3 s between up steps
    static constexpr unsigned UpAfterDownSamples = 20;   // >= 5 s up hold after a down step (x backoff)
    static constexpr unsigned MaxUpBackoff = 8;
    static constexpr unsigned GpuIdleSamples = 8;        // boosted but not busy for 2 s: give it back
    static constexpr unsigned PrematureUpSamples = 24;   // a down within 6 s of an up was premature
    static constexpr int32_t UpBandC = 3;                // raise only this far under the target...
    static constexpr int32_t UpPredictBandC = 2;         // ...and the prediction this far under
    static constexpr int32_t FastUpBandC = 10;           // 100 MHz GPU steps while this far under
    static constexpr int32_t DownBandC = 1;              // slow filter within this of the target: lower
    static constexpr int32_t FastOverC = 1;              // or the fast filter this far over it
    static constexpr int32_t PredictDownC = 3;           // predicted this far over the target: lower
    static constexpr int32_t PredictNearC = 4;           // ...only once within this of the target
    static constexpr int32_t SpikeC = 8;                 // instantaneous this far over: fast down
    static constexpr int32_t ReleaseBandC = 5;           // release turbo only this far under the target
    static constexpr int32_t HorizonS = 4;               // prediction horizon
    static constexpr uint32_t PowerMarginMw = 1000;      // raise only this far under the power limit
    static constexpr uint32_t GpuStepMhz = 50;
    static constexpr unsigned FloorBins = 3;             // floor = capTop - 3 (20 on the Dell)
    static constexpr int32_t MinTargetC = 50, MaxTargetC = 82; // target + SpikeC stays at or under the 90 C trip
    static constexpr uint32_t MinPkgLimitMw = 5000, MaxPkgLimitMw = 25000;
};

struct BalanceFilter {
    bool seeded = false;
    int32_t fast16 = 0, slow16 = 0; // package temperature EWMAs (alpha 1/4, 1/16), x16
    uint32_t pkgMw = 0, gtMw = 0;   // EWMA alpha 1/8 (~2 s)
    uint32_t busy = 0, c0 = 0;      // EWMA alpha 1/4 (~1 s)
    int32_t Temp() const { return (fast16 + 8) / 16; }
    int32_t Slow() const { return (slow16 + 8) / 16; }
    // Slope from the EWMA lag difference: a ramp of r C/s leaves the slow
    // filter (tau 3.75 s) 3 s further behind than the fast one (tau 0.75 s).
    // Predicted = fast + slope * (horizon + fast lag).
    int32_t Predicted() const
    {
        const int32_t d = fast16 - slow16;
        return (fast16 + d * (BalanceLaw::HorizonS * 4 + 3) / 12 + 8) / 16;
    }
};

inline bool BalanceConfigValid(const BalanceConfig& c)
{
    return c.targetC >= BalanceLaw::MinTargetC && c.targetC <= BalanceLaw::MaxTargetC &&
           c.pkgLimitMw >= BalanceLaw::MinPkgLimitMw && c.pkgLimitMw <= BalanceLaw::MaxPkgLimitMw;
}

// Derive limits from the PERF_CTL owner's range and the fused GPU caps.
inline BalanceLimits BalanceMakeLimits(unsigned minRatio, unsigned capTop, uint32_t gpuBaseMhz, uint32_t gpuRp0Mhz)
{
    BalanceLimits l{};
    l.minRatio = minRatio;
    l.capTop = capTop < minRatio ? minRatio : capTop;
    l.floorRatio = l.capTop > minRatio + BalanceLaw::FloorBins ? l.capTop - BalanceLaw::FloorBins : minRatio;
    l.lowestRatio = l.floorRatio * 2 / 3 > minRatio ? l.floorRatio * 2 / 3 : minRatio;
    l.gpuBaseMhz = gpuBaseMhz;
    l.gpuMaxMhz = gpuRp0Mhz > gpuBaseMhz ? gpuRp0Mhz - gpuRp0Mhz % BalanceLaw::GpuStepMhz : gpuBaseMhz;
    if (l.gpuMaxMhz < gpuBaseMhz) l.gpuMaxMhz = gpuBaseMhz;
    return l;
}

// Render-worker occupancy from cumulative busy ticks against the sampler's
// own TSC, so an idle worker (which stops publishing) reads as 0, not stale.
struct BalanceBusyMeter {
    bool primed = false;
    uint64_t render = 0, tsc = 0, resets = 0;
    // Returns false when this call only (re)primed the meter.
    bool Update(uint64_t renderTicks, uint64_t resetCount, uint64_t nowTsc, uint32_t& permille)
    {
        const bool ok = primed && resetCount == resets && renderTicks >= render && nowTsc > tsc;
        if (ok) {
            const uint64_t busy = renderTicks - render, span = nowTsc - tsc;
            permille = busy >= span ? 1000u : uint32_t(busy * 1000 / span);
        }
        primed = true;
        render = renderTicks;
        resets = resetCount;
        tsc = nowTsc;
        return ok;
    }
};

struct BalanceState {
    BalanceMode mode = BalanceMode::Light;
    unsigned cap = 0;          // 0 = released
    uint32_t gpuMhz = 0;       // requested render clock (gpuBaseMhz when not boosting)
    BalanceFilter filter{};
    BalanceReason lastReason = BalanceReason::None;
    BalanceAction lastAction = BalanceAction::None;
    BalanceReason lastStepReason = BalanceReason::None;
    unsigned settle = 0, sinceStep = 0xffff, heavyRun = 0, lightRun = 0, gpuIdleRun = 0;
    int8_t lastDir = 0;        // direction of the last ladder step
    unsigned upBackoff = 1, lightHold = BalanceLaw::LightSamples;
    bool tripSeen = false, tripPending = false;
    uint64_t lastReleaseMs = 0, lastBackoffMs = 0, lastStepMs = 0;
    uint64_t samples = 0, engages = 0, releases = 0, reengages = 0;
    uint64_t cpuDown = 0, cpuUp = 0, gpuDown = 0, gpuUp = 0, gpuIdleDown = 0;
    uint64_t spikes = 0, trips = 0, reversals = 0, prematureUps = 0, belowFloor = 0;
    unsigned minCapSeen = 0;    // lowest cap since enable (0 = never capped)
    uint32_t maxGpuSeen = 0;

    void Reset(const BalanceLimits& l) { *this = {}; gpuMhz = l.gpuBaseMhz; }

    // Render-clock overlay for the kernel: 0 = no override (the default
    // request applies). Only heavy mode boosts.
    uint32_t GpuOverlay(const BalanceLimits& l) const
    {
        return mode == BalanceMode::Heavy && gpuMhz > l.gpuBaseMhz ? gpuMhz : 0;
    }
};

namespace balance_detail {

inline void Note(BalanceState& st, const BalanceLimits& l, uint64_t now, int dir, BalanceReason why)
{
    using L = BalanceLaw;
    if (dir && st.lastDir && dir != st.lastDir) ++st.reversals;
    if (dir < 0 && st.lastDir > 0 && st.sinceStep < L::PrematureUpSamples) {
        ++st.prematureUps;
        if (st.upBackoff < L::MaxUpBackoff) st.upBackoff *= 2;
        st.lastBackoffMs = now;
    }
    if (dir) st.lastDir = int8_t(dir);
    st.sinceStep = 0;
    st.settle = L::SettleSamples;
    st.lastStepMs = now;
    st.lastStepReason = why;
    if (st.cap && (!st.minCapSeen || st.cap < st.minCapSeen)) st.minCapSeen = st.cap;
    if (st.gpuMhz > st.maxGpuSeen) st.maxGpuSeen = st.gpuMhz;
    if (st.cap && st.cap < l.floorRatio) ++st.belowFloor;
}

// One rung down. Returns the action taken.
inline BalanceAction Lower(BalanceState& st, const BalanceLimits& l, uint32_t gpuBins = 1)
{
    if (st.cap > l.floorRatio) { --st.cap; ++st.cpuDown; return BalanceAction::CpuDown; }
    if (st.gpuMhz > l.gpuBaseMhz) {
        const uint32_t step = BalanceLaw::GpuStepMhz * gpuBins;
        st.gpuMhz = st.gpuMhz > l.gpuBaseMhz + step ? st.gpuMhz - step : l.gpuBaseMhz;
        ++st.gpuDown;
        return BalanceAction::GpuDown;
    }
    if (st.cap > l.lowestRatio) { --st.cap; ++st.cpuDown; return BalanceAction::CpuDown; }
    return BalanceAction::AtFloor;
}

} // namespace balance_detail

// One controller step at nowMs. Updates st.cap / st.gpuMhz.
inline BalanceReason BalanceDecide(const BalanceConfig& cfg, const BalanceLimits& l, BalanceState& st,
                                   const BalanceSample& s, uint64_t nowMs)
{
    using L = BalanceLaw;
    using namespace balance_detail;
    auto done = [&](BalanceReason why, BalanceAction what = BalanceAction::None) {
        st.lastReason = why;
        st.lastAction = what;
        return why;
    };
    if (!s.tempValid || !s.loadValid) return done(BalanceReason::NoSample);
    ++st.samples;
    if (st.gpuMhz < l.gpuBaseMhz || st.gpuMhz > l.gpuMaxMhz) st.gpuMhz = l.gpuBaseMhz;
    if (st.cap && (st.cap < l.minRatio || st.cap > l.capTop)) st.cap = st.cap < l.minRatio ? l.minRatio : l.capTop;

    auto& f = st.filter;
    const int32_t t16 = s.pkgC * 16;
    if (!f.seeded) {
        f.seeded = true;
        f.fast16 = f.slow16 = t16;
        f.pkgMw = s.pkgMw;
        f.gtMw = s.gtMw;
        f.busy = s.gpuBusyPermille;
        f.c0 = s.c0Pct;
    } else {
        f.fast16 += (t16 - f.fast16) / 4;
        f.slow16 += (t16 - f.slow16) / 16;
        if (s.powerValid) {
            f.pkgMw = uint32_t(int64_t(f.pkgMw) + (int64_t(s.pkgMw) - int64_t(f.pkgMw)) / 8);
            f.gtMw = uint32_t(int64_t(f.gtMw) + (int64_t(s.gtMw) - int64_t(f.gtMw)) / 8);
        }
        f.busy = uint32_t(int64_t(f.busy) + (int64_t(s.gpuBusyPermille) - int64_t(f.busy)) / 4);
        f.c0 = uint32_t(int64_t(f.c0) + (int64_t(s.c0Pct) - int64_t(f.c0)) / 4);
    }
    if (st.sinceStep < 0xffff) ++st.sinceStep;
    // A trip latches for >= 5 s: react once to its edge, then hold all raises until it clears.
    const bool tripEdge = s.gpuTripped && !st.tripSeen;
    if (tripEdge) { ++st.trips; st.tripPending = true; }
    st.tripSeen = s.gpuTripped;
    if (st.lastBackoffMs && nowMs - st.lastBackoffMs >= L::CalmMs) {
        if (st.upBackoff > 1) st.upBackoff /= 2;
        if (st.lightHold > L::LightSamples) st.lightHold = st.lightHold / 2 > L::LightSamples ? st.lightHold / 2 : L::LightSamples;
        st.lastBackoffMs = st.upBackoff > 1 || st.lightHold > L::LightSamples ? nowMs : 0;
    }

    const int32_t T = cfg.targetC, temp = f.Temp(), pred = f.Predicted();
    const bool heavyNow = f.busy >= L::BusyHeavyPermille || f.c0 >= L::C0HeavyPct;
    const bool lightNow = f.busy < L::BusyLightPermille && f.c0 < L::C0LightPct;
    const bool powerOk = !s.powerValid || f.pkgMw + L::PowerMarginMw <= cfg.pkgLimitMw;

    if (st.mode == BalanceMode::Light) {
        st.lightRun = 0;
        st.heavyRun = heavyNow ? st.heavyRun + 1 : 0;
        if (st.heavyRun >= L::EnterSamples) {
            st.mode = BalanceMode::Heavy;
            st.heavyRun = 0;
            st.gpuIdleRun = 0;
            ++st.engages;
            if (st.lastReleaseMs && nowMs - st.lastReleaseMs < L::ReengageMs) {
                ++st.reengages;
                if (st.lightHold < L::MaxLightSamples)
                    st.lightHold = st.lightHold * 2 < L::MaxLightSamples ? st.lightHold * 2 : L::MaxLightSamples;
                st.lastBackoffMs = nowMs;
            }
            if (!st.cap) st.cap = l.capTop;
            st.gpuMhz = l.gpuBaseMhz;
            st.lastDir = 0;
            Note(st, l, nowMs, 0, BalanceReason::LoadHeavy);
            return done(BalanceReason::LoadHeavy, BalanceAction::Engage);
        }
        // Light load runs at the firmware request (turbo), unless the package
        // is hot: then hold max non-turbo - 1 until it cools.
        // (Filtered only: brief light-load turbo bursts are the default behaviour.)
        if (!st.cap && temp >= T - L::DownBandC) {
            st.cap = l.capTop;
            Note(st, l, nowMs, 0, BalanceReason::HoldHot);
            return done(BalanceReason::HoldHot, BalanceAction::CpuCapHot);
        }
        if (st.cap && st.sinceStep >= L::UpSamples && temp <= T - L::ReleaseBandC && pred <= T - L::UpPredictBandC) {
            st.cap = 0;
            ++st.releases;
            st.lastReleaseMs = nowMs;
            Note(st, l, nowMs, 0, BalanceReason::LoadLight);
            return done(BalanceReason::LoadLight, BalanceAction::Release);
        }
        return done(st.cap ? BalanceReason::HoldHot : BalanceReason::Band);
    }

    // Heavy.
    st.lightRun = lightNow ? st.lightRun + 1 : 0;
    if (st.lightRun >= st.lightHold) {
        st.mode = BalanceMode::Light;
        st.lightRun = 0;
        st.gpuMhz = l.gpuBaseMhz;
        st.lastDir = 0;
        const bool cool = temp <= T - L::ReleaseBandC && pred <= T - L::UpPredictBandC;
        if (cool) {
            st.cap = 0;
            ++st.releases;
            st.lastReleaseMs = nowMs;
        }
        // Hot but light: keep the cap (no turbo bursts); light mode releases it once cool.
        Note(st, l, nowMs, 0, BalanceReason::LoadLight);
        return done(BalanceReason::LoadLight, cool ? BalanceAction::Release : BalanceAction::CpuCapHot);
    }
    if (st.settle) { --st.settle; return done(BalanceReason::Settling); }

    if (st.tripPending || s.pkgC >= T + L::SpikeC) {
        // Fast down: drop the GPU boost two bins and one CPU bin above the
        // floor in the same sample; below the floor only with no boost left.
        const auto why = st.tripPending ? BalanceReason::Trip : BalanceReason::Spike;
        if (!st.tripPending) ++st.spikes;
        st.tripPending = false;
        BalanceAction what = BalanceAction::None;
        if (st.gpuMhz > l.gpuBaseMhz) {
            const uint32_t step = 2 * L::GpuStepMhz;
            st.gpuMhz = st.gpuMhz > l.gpuBaseMhz + step ? st.gpuMhz - step : l.gpuBaseMhz;
            ++st.gpuDown;
            what = BalanceAction::GpuDown;
        }
        if (st.cap > l.floorRatio) {
            --st.cap; ++st.cpuDown;
            if (what == BalanceAction::None) what = BalanceAction::CpuDown;
        }
        if (what == BalanceAction::None) what = Lower(st, l);
        if (what == BalanceAction::AtFloor) return done(why, what);
        Note(st, l, nowMs, -1, why);
        return done(why, what);
    }

    // The slow filter carries the "at the target" decision (noise and the
    // fast thermal response of our own last step stay out of it); the fast
    // filter only when clearly over, and the prediction supplies the lead.
    const bool overTemp = f.Slow() >= T - L::DownBandC || temp >= T + L::FastOverC;
    const bool overPred = temp >= T - L::PredictNearC && pred >= T + L::PredictDownC;
    const bool overPower = s.powerValid && f.pkgMw > cfg.pkgLimitMw && st.sinceStep >= L::PowerDownSamples;
    if (((overTemp || overPred) && st.sinceStep >= L::DownSamples) || overPower) {
        const auto why = overTemp ? BalanceReason::OverTemp : overPred ? BalanceReason::Predicted : BalanceReason::OverPower;
        const auto what = Lower(st, l);
        if (what == BalanceAction::AtFloor) return done(why, what);
        Note(st, l, nowMs, -1, why);
        return done(why, what);
    }
    if (overTemp || overPred) return done(BalanceReason::Wait);

    // Boost the GPU only while it is the bottleneck.
    st.gpuIdleRun = f.busy < L::BusyLightPermille ? st.gpuIdleRun + 1 : 0;
    if (st.gpuMhz > l.gpuBaseMhz && st.gpuIdleRun >= L::GpuIdleSamples && st.sinceStep >= L::DownSamples) {
        st.gpuMhz -= L::GpuStepMhz;
        if (st.gpuMhz < l.gpuBaseMhz) st.gpuMhz = l.gpuBaseMhz;
        st.gpuIdleRun = 0;
        ++st.gpuIdleDown;
        Note(st, l, nowMs, 0, BalanceReason::GpuIdle);
        return done(BalanceReason::GpuIdle, BalanceAction::GpuDown);
    }

    // Nothing rises while the 90 C fallback is latched.
    if (s.gpuTripped) return done(BalanceReason::Trip);
    const unsigned upHold = st.lastDir < 0 ? L::UpAfterDownSamples * st.upBackoff : L::UpSamples;
    if (st.sinceStep < upHold) return done(BalanceReason::Wait);
    const int32_t hi = pred > temp ? pred : temp;
    if (temp > T - L::UpBandC || hi > T - L::UpPredictBandC || !powerOk) return done(BalanceReason::Band);

    if (st.cap < l.floorRatio) {
        ++st.cap; ++st.cpuUp;
        Note(st, l, nowMs, +1, BalanceReason::Headroom);
        return done(BalanceReason::Headroom, BalanceAction::CpuUp);
    }
    if (st.gpuMhz < l.gpuMaxMhz && f.busy >= L::BusyHeavyPermille) {
        const bool far = temp <= T - L::FastUpBandC && hi <= T - L::FastUpBandC + 2;
        // Feed-forward: GT power grows ~f^2.5-f^3 here; charge the increase
        // 5/4 to the package (GT-driven ring/LLC traffic lands in PP0).
        auto ffOk = [&](uint32_t next) {
            if (!s.powerValid || !st.gpuMhz) return true;
            const uint64_t c = st.gpuMhz, n = next;
            const uint64_t gtNext = uint64_t(f.gtMw) * n * n * n / (c * c * c);
            const uint64_t extra = (gtNext - f.gtMw) * 5 / 4;
            return f.pkgMw + extra + L::PowerMarginMw / 2 <= cfg.pkgLimitMw;
        };
        for (uint32_t bins = far ? 2 : 1; bins; --bins) {
            uint32_t next = st.gpuMhz + bins * L::GpuStepMhz;
            if (next > l.gpuMaxMhz) next = l.gpuMaxMhz;
            if (!ffOk(next)) continue;
            st.gpuMhz = next;
            ++st.gpuUp;
            Note(st, l, nowMs, +1, BalanceReason::Headroom);
            return done(BalanceReason::Headroom, BalanceAction::GpuUp);
        }
        // Over the power budget: the GPU stays; the CPU above the floor may still rise.
    }
    if (st.cap < l.capTop) {
        ++st.cap; ++st.cpuUp;
        Note(st, l, nowMs, +1, BalanceReason::Headroom);
        return done(BalanceReason::Headroom, BalanceAction::CpuUp);
    }
    return done(BalanceReason::Band, BalanceAction::AtTop);
}

} // namespace brook
