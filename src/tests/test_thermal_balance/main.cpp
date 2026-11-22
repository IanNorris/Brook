// Host test: combined CPU + GPU thermal balance law (thermal_balance.h), its
// PERF_CTL owner in external mode (cpu_power_budget.h) and the render clock
// balance overlay (intel_bdw_render_clock.h), against a synthetic plant.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include "thermal_balance.h"
#include "cpu_power_budget.h"
#include "intel_bdw_render_clock.h"
using namespace brook;

namespace {

constexpr uint32_t DellCaps = 0x060613; // RPn 300, RP1 300, RP0 950
constexpr uint64_t PeriodMs = 250;

BalanceLimits Dell() { return BalanceMakeLimits(8, 23, BdwClockBootMhz, 950, 300); }

// Plant: Dell XPS 13 9343 under the choppy 480p AV1 clip, fitted to the 21 Nov
// int86/int87 histories. First-order (two time constant) package thermal
// model fed by CPU, GT and uncore power, with the GPU's 90 C trip emulated by
// the real BdwClockNoteTemp. PP0 includes GT-driven ring/LLC power
// (ringK * GT), the effect that made the pure PP0 budget starve the CPU.
struct Plant {
    double ambient = 50, rSlow = 1.5, rFast = 3.0, tauSlow = 30, tauFast = 1.0;
    double ringK = 1.2, uncoreW = 4.8, hotspotK = 0.5;
    bool bursts = true;   // released turbo bursts (cores ~13 W for 0.5-1 s)
    double gpuLoad = 1.0; // 1 = GPU-bound clip; 0 = idle desktop
    double cpuLoad = 1.0;
    // State.
    double pFast = 0, pSlow = 0;
    uint32_t rng = 12345;
    unsigned burstLeft = 0;
    uint64_t nowMs = 0;
    BdwClockState clock{};
    // Last outputs.
    double fps = 0, tempC = 0, pkgW = 0, gtW = 0, pp0W = 0, cpuGhz = 0;
    int instantC = 0;
    uint32_t gpuMhz = 0, busyPermille = 0, c0Pct = 0;

    Plant() { clock.lastCaps = DellCaps; }
    double Noise()
    {
        rng = rng * 1103515245u + 12345u;
        return double((rng >> 16) & 0x7fff) / 32767.0;
    }
    void Settle(double w) { pFast = pSlow = w; }

    // Advance 250 ms with PERF_CTL cap (0 = firmware/turbo) and GPU overlay.
    BalanceSample Step(unsigned cap, uint32_t overlayMhz)
    {
        nowMs += PeriodMs;
        BdwClockSetBalance(clock, overlayMhz);
        gpuMhz = BdwClockEffectiveBin(clock) * BdwClockBinMhz;
        bool burst = false;
        if (!cap) {
            if (burstLeft) { --burstLeft; burst = true; }
            else if (bursts && Noise() < 0.08 * cpuLoad) { burstLeft = 1 + unsigned(Noise() * 3); burst = true; }
            cpuGhz = burst ? 2.9 : 2.7;
        } else {
            cpuGhz = cap / 10.0;
        }
        // Frame: app job 8 ms + 25 ms at 600 MHz (33 @ 600, 23.8 @ 950); CPU
        // (dav1d + Chromium) 14 ms at 2.3 GHz; pipelined.
        // Compositor job adds 4 + 10 ms at 600 MHz (21 fps @ 600, ~29 @ 950).
        const double gpuMs = gpuLoad > 0 ? 12 + 35.0 * 600 / gpuMhz : 0;
        const double cpuMs = 14 * 2.3 / cpuGhz * (cpuLoad > 0 ? 1 : 0);
        const double frameMs = gpuMs > cpuMs ? gpuMs : cpuMs;
        fps = frameMs > 0 ? 1000 / frameMs : 0;
        double busy = gpuLoad > 0 ? gpuMs * fps / 1000 * gpuLoad : 0.02;
        if (busy > 1) busy = 1;
        double c0 = cpuLoad > 0 ? cpuMs * fps / 1000 * 0.35 * cpuLoad + 0.05 : 0.03;
        if (burst) c0 += 0.4;
        if (c0 > 1) c0 = 1;
        gtW = 0.05 + (0.10 + 0.40 * std::pow(gpuMhz / 600.0, 2.8)) * (busy > 0.05 ? 1 : 0.1);
        // Cores: a large frequency-independent C0 part plus ~f*V^2 dynamic
        // power (energy per frame ~f^2, as slower CPUs spend longer in C0).
        const double coresW = 0.8 + 1.5 * std::pow(cpuGhz / 2.3, 2) * (c0 / 0.25) + (burst ? 7.0 : 0);
        pp0W = coresW + ringK * gtW;
        pkgW = pp0W + gtW + uncoreW;
        const double dt = PeriodMs / 1000.0;
        pFast += (pkgW - pFast) * dt / tauFast;
        pSlow += (pkgW - pSlow) * dt / tauSlow;
        tempC = ambient + rSlow * pSlow + rFast * (pFast - pSlow) + hotspotK * (coresW - 3) * (burst ? 1 : 0.2);
        instantC = int(std::lround(tempC + (Noise() - 0.5) * 2.0));
        if (instantC > 100) instantC = 100; // PROCHOT territory; the plant is not modelled past it
        BdwClockNoteTemp(clock, instantC, nowMs);
        busyPermille = uint32_t(busy * 1000);
        c0Pct = uint32_t(c0 * 100);
        BalanceSample s{};
        s.tempValid = s.powerValid = s.loadValid = true;
        s.pkgC = instantC;
        s.pkgMw = uint32_t(pkgW * 1000);
        s.gtMw = uint32_t(gtW * 1000);
        s.c0Pct = c0Pct;
        s.gpuBusyPermille = busyPermille;
        s.gpuTripped = clock.tripped != 0;
        return s;
    }
};

struct RunStats {
    double fpsSum = 0, maxFiltered = -1e9, maxInstant = -1e9;
    uint64_t n = 0, trips = 0, gpuChanges = 0, capChanges = 0;
    unsigned minCap = 255;
    uint32_t minGpu = 100000, maxGpu = 0;
    double Fps() const { return n ? fpsSum / n : 0; }
};

// Runs the balance law on the plant for `seconds`; stats cover only samples
// at or after `fromS` (to skip the transient).
RunStats RunBalance(Plant& p, BalanceState& st, const BalanceConfig& cfg, const BalanceLimits& l,
                    double seconds, double fromS = 0, bool checkLadder = true)
{
    RunStats r{};
    const uint64_t start = p.nowMs;
    uint64_t trips0 = p.clock.trips;
    unsigned lastCap = st.cap;
    uint32_t lastGpu = st.gpuMhz;
    for (uint64_t t = 0; t < uint64_t(seconds * 1000); t += PeriodMs) {
        const auto s = p.Step(st.cap, st.GpuOverlay(l));
        BalanceDecide(cfg, l, st, s, p.nowMs);
        if (checkLadder) {
            // Priority invariant: never below the floor while the GPU is boosted.
            assert(!(st.cap && st.cap < l.floorRatio && st.gpuMhz > l.gpuBaseMhz));
            assert(st.gpuMhz >= l.gpuBaseMhz && st.gpuMhz <= l.gpuMaxMhz);
            assert(!st.cap || (st.idle && st.cap == l.idleRatio) || (st.cap >= l.lowestRatio && st.cap <= l.capTop));
        }
        if (double(p.nowMs - start) / 1000 < fromS) { lastCap = st.cap; lastGpu = st.gpuMhz; continue; }
        ++r.n;
        r.fpsSum += p.fps;
        if (st.filter.Temp() > r.maxFiltered) r.maxFiltered = st.filter.Temp();
        if (p.instantC > r.maxInstant) r.maxInstant = p.instantC;
        if (st.cap && st.cap < r.minCap) r.minCap = st.cap;
        if (st.gpuMhz < r.minGpu) r.minGpu = st.gpuMhz;
        if (st.gpuMhz > r.maxGpu) r.maxGpu = st.gpuMhz;
        if (st.cap != lastCap) ++r.capChanges;
        if (st.gpuMhz != lastGpu) ++r.gpuChanges;
        lastCap = st.cap;
        lastGpu = st.gpuMhz;
    }
    r.trips = p.clock.trips - trips0;
    return r;
}

// Default behaviour (balance off): firmware turbo, fixed 600 MHz.
RunStats RunDefault(Plant& p, double seconds, double fromS)
{
    RunStats r{};
    const uint64_t start = p.nowMs, trips0 = p.clock.trips;
    for (uint64_t t = 0; t < uint64_t(seconds * 1000); t += PeriodMs) {
        p.Step(0, 0);
        if (double(p.nowMs - start) / 1000 < fromS) continue;
        ++r.n;
        r.fpsSum += p.fps;
        if (p.instantC > r.maxInstant) r.maxInstant = p.instantC;
    }
    r.trips = p.clock.trips - trips0;
    return r;
}

BalanceSample Sample(int c, uint32_t busy = 950, uint32_t pkgMw = 10000, uint32_t gtMw = 1000, uint32_t c0 = 30)
{
    BalanceSample s{};
    s.tempValid = s.powerValid = s.loadValid = true;
    s.pkgC = c;
    s.pkgMw = pkgMw;
    s.gtMw = gtMw;
    s.c0Pct = c0;
    s.gpuBusyPermille = busy;
    return s;
}

void TestLimitsAndMeter()
{
    const auto l = Dell();
    assert(l.minRatio == 8 && l.capTop == 23 && l.floorRatio == 20 && l.lowestRatio == 13);
    assert(l.gpuBaseMhz == 600 && l.gpuMaxMhz == 950);
    assert(l.idleRatio == 8 && l.gpuIdleMhz == 450);
    // Unknown caps: no GPU boost range, no idle render clock.
    const auto n = BalanceMakeLimits(8, 23, 600, 600);
    assert(n.gpuMaxMhz == 600 && n.gpuIdleMhz == 0);
    const auto tight = BalanceMakeLimits(8, 10, 600, 950);
    assert(tight.floorRatio == 8 && tight.idleRatio == 8);
    // The real Dell: LFM 5 -> idle cap 8; RPn above 450 -> no idle render clock.
    assert(BalanceMakeLimits(5, 23, 600, 950, 300).idleRatio == 8);
    assert(BalanceMakeLimits(12, 23, 600, 950, 500).idleRatio == 12);
    assert(BalanceMakeLimits(5, 23, 600, 950, 500).gpuIdleMhz == 0);
    assert(BalanceConfigValid(BalanceConfig{}));
    assert(!BalanceConfigValid(BalanceConfig{90, 13000}));  // too close to the 90 C trip
    assert(!BalanceConfigValid(BalanceConfig{75, 30000}));

    BalanceBusyMeter m{};
    uint32_t pm = 7;
    assert(!m.Update(100, 0, 1000, pm) && pm == 7); // primes only
    assert(m.Update(600, 0, 2000, pm) && pm == 500);
    assert(m.Update(600, 0, 3000, pm) && pm == 0);  // idle worker: no new ticks
    assert(m.Update(5000, 0, 4000, pm) && pm == 1000); // clamped
    assert(!m.Update(10, 1, 5000, pm));             // stats reset: re-prime
    assert(m.Update(260, 1, 5500, pm) && pm == 500);
}

void TestOverlay()
{
    BdwClockState s{};
    s.lastCaps = DellCaps;
    assert(BdwClockEffectiveBin(s) == 12);           // fixed 600 default
    BdwClockSetBalance(s, 850);
    assert(BdwClockEffectiveBin(s) == 17);
    BdwClockLoadSample load{};
    assert(BdwClockSelectRequest(s, true, load, 2400000) == 17);
    BdwClockSetAuto(s);                              // overlay also wins over auto
    assert(BdwClockSelectRequest(s, false, load, 2400000) == 17);
    BdwClockSetMhz(s, 600);
    assert(BdwClockNoteTemp(s, 91, 1000) == 1);      // the 90 C trip still applies
    assert(BdwClockEffectiveBin(s) == 10 && s.balanceMhz == 850);
    BdwClockSetBalance(s, 0);
    assert(BdwClockNoteTemp(s, 60, 2000) == 0 && BdwClockNoteTemp(s, 60, 7000) == -1);
    assert(BdwClockEffectiveBin(s) == 12);           // back to the fixed request
    BdwClockSetBalance(s, 2000);
    assert(BdwClockEffectiveBin(s) == 19);           // clamped to RP0
}

void TestExternalOwner()
{
    CpuBudgetConfig ext{};
    ext.external = true;
    assert(CpuPowerBudget::Valid(ext));
    CpuBudgetConfig bad = ext;
    bad.tempC = 75;
    assert(!CpuPowerBudget::Valid(bad));
    uint8_t msr[4] = {29, 29, 29, 29};
    for (int ac = 0; ac < 2; ++ac) {
        CpuPowerBudget b{};
        assert(b.Start(0, 4, ext, 8, 24));
        assert(!b.SetCap(0, 23));                   // not active yet
        for (unsigned c = 0; c < 4; ++c)
            b.Tick(c, 1, true, [&] { return unsigned(msr[c]); }, [&](unsigned v) { msr[c] = uint8_t(v); return v; });
        assert(b.phase == CpuPowerBudget::Phase::Active && b.limits.capTop == 23);
        CpuBudgetConfig law{};
        law.coresMw = 3000;
        assert(!b.SetConfig(law));                  // the built-in law cannot take over
        b.Control(2, CpuBudgetSample{true, true, 9000, 90});
        assert(b.cap == 0);                         // Control is a no-op in external mode
        assert(b.SetCap(3, 23) && b.cap == 23);
        assert(b.SetCap(3, 40) && b.cap == 23);     // clamped to capTop
        assert(b.SetCap(3, 2) && b.cap == 8);       // clamped to minRatio
        assert(b.SetCap(3, 20) && b.cap == 20);
        for (unsigned c = 0; c < 4; ++c)
            b.Tick(c, 4, true, [&] { return unsigned(msr[c]); }, [&](unsigned v) { msr[c] = uint8_t(v); return v; });
        for (unsigned c = 0; c < 4; ++c) assert(msr[c] == 20);
        // Off (ac == 0) or AC loss (ac == 1, eligible false): restore every CPU.
        if (!ac) b.Cancel();
        for (unsigned c = 0; c < 4; ++c)
            b.Tick(c, 5, ac == 0, [&] { return unsigned(msr[c]); }, [&](unsigned v) { msr[c] = uint8_t(v); return v; });
        assert(b.phase == CpuPowerBudget::Phase::Off);
        assert(b.outcome == (ac ? CpuPowerBudget::Outcome::Ineligible : CpuPowerBudget::Outcome::Disabled));
        for (unsigned c = 0; c < 4; ++c) assert(msr[c] == 29);
    }
}

// Engage on heavy load, ramp the GPU while there is headroom, release on light load.
void TestRampAndRelease()
{
    const auto l = Dell();
    const BalanceConfig cfg{};
    Plant p{};
    p.Settle(7.7);
    BalanceState st{};
    st.Reset(l);
    auto r = RunBalance(p, st, cfg, l, 120, 60);
    assert(st.mode == BalanceMode::Heavy && st.engages == 1);
    assert(st.cap == l.capTop);                     // max non-turbo - 1
    assert(st.gpuMhz == 950 && st.GpuOverlay(l) == 950);
    assert(r.trips == 0 && r.maxFiltered < cfg.targetC);
    assert(st.cpuDown == 0 && st.reversals == 0);
    // Compare against the default policy on the same plant.
    Plant d{};
    d.Settle(7.7);
    const auto base = RunDefault(d, 120, 60);
    printf("ramp: balance %.1f fps (pkg max %.0f) vs default %.1f fps (pkg max %.0f, trips %llu)\n",
           r.Fps(), r.maxInstant, base.Fps(), base.maxInstant, (unsigned long long)base.trips);
    assert(r.Fps() > base.Fps() * 1.12);

    // Light load: release after the 3 s hold once cool; GPU back to the default request.
    p.gpuLoad = 0;
    p.cpuLoad = 0.2;
    RunBalance(p, st, cfg, l, 10);
    assert(st.mode == BalanceMode::Light && st.cap == 0 && st.GpuOverlay(l) == 0 && st.releases == 1);
    // Heavy again within 30 s: re-engage, release hold backs off.
    p.gpuLoad = 1;
    p.cpuLoad = 1;
    RunBalance(p, st, cfg, l, 5);
    assert(st.mode == BalanceMode::Heavy && st.engages == 2 && st.reengages == 1);
    assert(st.lightHold == 2 * BalanceLaw::LightSamples);
}

// Flapping load (5 s heavy / 4 s light): the release backoff stops turbo churn.
void TestFlappingLoad()
{
    const auto l = Dell();
    const BalanceConfig cfg{};
    Plant p{};
    p.Settle(7.7);
    BalanceState st{};
    st.Reset(l);
    for (int cycle = 0; cycle < 14; ++cycle) {
        p.gpuLoad = 1; p.cpuLoad = 1;
        RunBalance(p, st, cfg, l, 5);
        p.gpuLoad = 0; p.cpuLoad = 0.2;
        RunBalance(p, st, cfg, l, 4);
    }
    printf("flapping: %llu releases, %llu re-engages over 126 s, light hold %u ms\n",
           (unsigned long long)st.releases, (unsigned long long)st.reengages, st.lightHold * 250);
    assert(st.releases <= 3);
    assert(st.lightHold > BalanceLaw::LightSamples);
}

// A hotter plant (worse paste): settle under the target, no 90 C trip, CPU
// held at or above the floor, the GPU still boosted, and no oscillation.
void TestHotPlantSettles()
{
    const auto l = Dell();
    const BalanceConfig cfg{};
    Plant p{};
    p.rSlow = 3.0;
    p.rFast = 4.0;
    p.Settle(7.7);
    BalanceState st{};
    st.Reset(l);
    const auto r = RunBalance(p, st, cfg, l, 600, 180);
    printf("hot plant: %.1f fps, filtered max %.0f, instant max %.0f, cap %u..%u, gpu %u..%u, "
           "gpu changes %llu / cap changes %llu in 420 s, reversals %llu, backoff %u\n",
           r.Fps(), r.maxFiltered, r.maxInstant, r.minCap, st.cap, r.minGpu, r.maxGpu,
           (unsigned long long)r.gpuChanges, (unsigned long long)r.capChanges,
           (unsigned long long)st.reversals, st.upBackoff);
    assert(r.trips == 0 && r.maxInstant < BdwClockDefaultLimitC);
    assert(r.maxFiltered <= cfg.targetC + 1);
    assert(r.minCap >= l.floorRatio);
    assert(r.minGpu > l.gpuBaseMhz);               // still spending headroom on the GPU
    assert(r.gpuChanges + r.capChanges <= 6);       // converges: (almost) no steps at steady state
    assert(st.reversals <= 6);
    // The default policy on this plant trips the GPU.
    Plant d = Plant{};
    d.rSlow = 3.0;
    d.rFast = 4.0;
    d.Settle(7.7);
    const auto base = RunDefault(d, 600, 180);
    printf("hot plant default: %.1f fps, instant max %.0f, trips %llu\n", base.Fps(), base.maxInstant,
           (unsigned long long)base.trips);
    assert(base.trips > 0);
    assert(r.Fps() > base.Fps());
}

// Very hot plant: the CPU goes below the floor only after the GPU boost is gone.
void TestPriorityBelowFloor()
{
    const auto l = Dell();
    const BalanceConfig cfg{};
    Plant p{};
    p.rSlow = 3.6;
    p.rFast = 4.5;
    p.Settle(7.7);
    BalanceState st{};
    st.Reset(l);
    const auto r = RunBalance(p, st, cfg, l, 400, 200); // the ladder invariant is asserted per step
    printf("very hot plant: filtered max %.0f, cap %u (min %u), gpu %u, below-floor steps %llu\n",
           r.maxFiltered, st.cap, r.minCap, st.gpuMhz, (unsigned long long)st.belowFloor);
    assert(st.belowFloor > 0 && st.cap < l.floorRatio && st.gpuMhz == l.gpuBaseMhz);
    assert(r.minCap >= l.lowestRatio && l.lowestRatio == 13);
    assert(r.maxFiltered <= cfg.targetC + 2);
}

// Spike: an instantaneous reading >= target + 8 drops the GPU two bins and
// the CPU one bin in the same sample, without the usual down wait.
void TestSpikeFastDown()
{
    const auto l = Dell();
    const BalanceConfig cfg{};
    BalanceState st{};
    st.Reset(l);
    uint64_t now = 0;
    for (int i = 0; i < 8; ++i) BalanceDecide(cfg, l, st, Sample(60), now += 250);
    assert(st.mode == BalanceMode::Heavy && st.cap == 23 && st.gpuMhz == 600);
    // Far under the target: 100 MHz steps.
    for (int i = 0; i < 80 && st.gpuMhz < 950; ++i) BalanceDecide(cfg, l, st, Sample(60), now += 250);
    assert(st.gpuMhz == 950 && st.gpuUp <= 4);
    for (int i = 0; i < 12; ++i) BalanceDecide(cfg, l, st, Sample(62), now += 250);
    const auto down = st.gpuDown, cpu = st.cpuDown;
    assert(BalanceDecide(cfg, l, st, Sample(cfg.targetC + BalanceLaw::SpikeC), now += 250) == BalanceReason::Spike);
    assert(st.gpuMhz == 850 && st.cap == 22 && st.gpuDown == down + 1 && st.cpuDown == cpu + 1);
    assert(st.spikes == 1);
    assert(BalanceDecide(cfg, l, st, Sample(cfg.targetC + BalanceLaw::SpikeC), now += 250) == BalanceReason::Settling);
    // Spike back-off: at most one fast-down per second (the step needs time to act).
    for (unsigned i = 2; i < BalanceLaw::SpikeSamples; ++i) {
        assert(BalanceDecide(cfg, l, st, Sample(cfg.targetC + BalanceLaw::SpikeC), now += 250) == BalanceReason::Wait);
        assert(st.gpuMhz == 850 && st.cap == 22);
    }
    assert(st.spikeHolds == BalanceLaw::SpikeSamples - 2);
    assert(BalanceDecide(cfg, l, st, Sample(cfg.targetC + BalanceLaw::SpikeC), now += 250) == BalanceReason::Spike);
    assert(st.gpuMhz == 750 && st.cap == 21);
    assert(st.upBackoff >= 4);                      // each spike backs off the next raise
    // A 90 C trip seen from the render clock acts like a spike and is counted.
    auto trip = Sample(70);
    trip.gpuTripped = true;
    BalanceDecide(cfg, l, st, trip, now += 250);
    assert(BalanceDecide(cfg, l, st, trip, now += 250) == BalanceReason::Trip);
    assert(st.trips == 1 && st.gpuMhz == 650 && st.cap == 20);
    // While the trip stays latched: no repeated trip fast-downs (the filters
    // still lower normally while hot), and nothing rises.
    for (int i = 0; i < 40; ++i) BalanceDecide(cfg, l, st, trip, now += 250);
    assert(st.trips == 1 && st.spikes == 2 && st.cap >= l.floorRatio - 1);
    const auto latchedGpu = st.gpuMhz, latchedCap = st.cap;
    trip.pkgC = 55;
    for (int i = 0; i < 200; ++i) BalanceDecide(cfg, l, st, trip, now += 250);
    assert(st.gpuMhz == latchedGpu && st.cap == latchedCap); // nothing rises while latched
    trip.gpuTripped = false;
    // Two spikes and a trip back off the next raise (up hold 5 s x backoff,
    // halved per calm minute): the first raise comes within ~45 s.
    for (int i = 0; i < 180 && st.gpuMhz <= latchedGpu; ++i) BalanceDecide(cfg, l, st, trip, now += 250);
    assert(st.gpuMhz > latchedGpu);
    // Up is held off after a down step.
    BalanceDecide(cfg, l, st, Sample(cfg.targetC + BalanceLaw::SpikeC), now += 250);
    BalanceDecide(cfg, l, st, Sample(cfg.targetC + BalanceLaw::SpikeC), now += 250);
    const auto heldGpu = st.gpuMhz, heldCap = st.cap;
    for (int i = 0; i < int(BalanceLaw::UpAfterDownSamples) - 2; ++i) {
        BalanceDecide(cfg, l, st, Sample(55), now += 250);
        assert(st.gpuMhz == heldGpu && st.cap == heldCap);
    }
}

// Near the target: lower before reaching it (filtered >= target - 1), CPU to
// the floor first, then the GPU; never below the floor while boosted.
void TestBackOffNearTarget()
{
    const auto l = Dell();
    const BalanceConfig cfg{};
    BalanceState st{};
    st.Reset(l);
    uint64_t now = 0;
    for (int i = 0; i < 100; ++i) BalanceDecide(cfg, l, st, Sample(60), now += 250);
    assert(st.gpuMhz == 950 && st.cap == 23);
    BalanceAction order[16]{};
    unsigned n = 0;
    for (int i = 0; i < 400 && n < 16; ++i) {
        const auto before = st.lastStepMs;
        BalanceDecide(cfg, l, st, Sample(cfg.targetC - 1), now += 250);
        if (st.lastStepMs != before) order[n++] = st.lastAction;
        assert(st.cap >= l.floorRatio || st.gpuMhz == l.gpuBaseMhz);
    }
    // 23 -> 20 first, then 950 -> 600 in 50 MHz bins, then below the floor.
    for (unsigned i = 0; i < 3; ++i) assert(order[i] == BalanceAction::CpuDown);
    for (unsigned i = 3; i < 10; ++i) assert(order[i] == BalanceAction::GpuDown);
    assert(order[10] == BalanceAction::CpuDown);
    assert(st.cap < l.floorRatio && st.gpuMhz == 600 && st.belowFloor > 0);
    // Rising temperature with lead: predicted >= target + 3 lowers while still 4 C under.
    BalanceState r{};
    r.Reset(l);
    now = 0;
    for (int i = 0; i < 100; ++i) BalanceDecide(cfg, l, r, Sample(58), now += 250);
    const auto steps = r.cpuDown;
    bool predicted = false;
    for (int t = 58; t <= 72 && !predicted; ++t)
        for (int k = 0; k < 2; ++k)
            if (BalanceDecide(cfg, l, r, Sample(t), now += 250) == BalanceReason::Predicted) predicted = true;
    assert(predicted && r.cpuDown > steps && r.filter.Temp() < cfg.targetC - 1);
}

// Package power: the sanity limit lowers even when cool, and the feed-forward
// refuses a GPU step that would cross it.
void TestPowerLimit()
{
    const auto l = Dell();
    const BalanceConfig cfg{};
    BalanceState st{};
    st.Reset(l);
    uint64_t now = 0;
    // Package and GT power follow the requested clock: pkg = 9 W + 2.25 * GT.
    auto at = [&](int c, uint32_t base) {
        const double gt = 520 * std::pow(st.gpuMhz / 600.0, 2.8);
        return Sample(c, 950, uint32_t(base + 2.25 * gt), uint32_t(gt));
    };
    for (int i = 0; i < 400; ++i) BalanceDecide(cfg, l, st, at(60, 9000), now += 250);
    printf("power limit: gpu %u, filtered pkg %u mW\n", st.gpuMhz, st.filter.pkgMw);
    assert(st.gpuMhz < 950 && st.gpuMhz > 600);   // feed-forward stopped short of the limit
    assert(st.filter.pkgMw <= cfg.pkgLimitMw && st.cap == 23);
    for (int i = 0; i < 40; ++i) BalanceDecide(cfg, l, st, at(60, 11500), now += 250);
    assert(st.lastStepReason == BalanceReason::OverPower && st.cap < 23);
}

// GPU boost is only kept while the GPU is the bottleneck.
void TestGpuIdleGivesBack()
{
    const auto l = Dell();
    const BalanceConfig cfg{};
    BalanceState st{};
    st.Reset(l);
    uint64_t now = 0;
    for (int i = 0; i < 100; ++i) BalanceDecide(cfg, l, st, Sample(60), now += 250);
    assert(st.gpuMhz == 950);
    // CPU-heavy, GPU not busy: stay engaged (C0 high) but step the GPU back.
    for (int i = 0; i < 200; ++i) BalanceDecide(cfg, l, st, Sample(60, 300, 9000, 600, 85), now += 250);
    assert(st.mode == BalanceMode::Heavy && st.gpuMhz == 600 && st.gpuIdleDown == 7 && st.cap == 23);
    assert(st.GpuOverlay(l) == 0);
}

// Light load but hot: cap at max non-turbo - 1 instead of turbo, release when cool.
void TestLightHot()
{
    const auto l = Dell();
    const BalanceConfig cfg{};
    BalanceState st{};
    st.Reset(l);
    uint64_t now = 0;
    BalanceDecide(cfg, l, st, Sample(60, 100, 6000, 100, 10), now += 250);
    assert(st.cap == 0);
    for (int i = 0; i < 6; ++i) BalanceDecide(cfg, l, st, Sample(80, 100, 6000, 100, 10), now += 250);
    assert(st.mode == BalanceMode::Light && st.cap == 23 && st.lastAction != BalanceAction::Engage);
    for (int i = 0; i < 80; ++i) BalanceDecide(cfg, l, st, Sample(60, 100, 6000, 100, 10), now += 250);
    assert(st.cap == 0 && st.releases == 1);
    // Invalid input changes nothing.
    auto bad = Sample(95);
    bad.tempValid = false;
    assert(BalanceDecide(cfg, l, st, bad, now += 250) == BalanceReason::NoSample && st.cap == 0);
}

// The old failure mode: PP0 includes GT-driven ring/LLC power, so with the GPU
// boosted a pure PP0 budget keeps cutting the CPU (to ~1.5 GHz on the Dell,
// ratio 8 at the extreme) although the package is cool. The balance law, fed
// the same plant, keeps the CPU at max non-turbo - 1 and the GPU at RP0.
void TestPp0CouplingFailureMode()
{
    // Old law: 3.5 W PP0 budget (the Dell trial), GPU fixed 950.
    Plant p{};
    p.Settle(9.0);
    p.bursts = false;
    CpuBudgetConfig cfg{};
    cfg.coresMw = 3500;
    CpuBudgetLimits lim{8, 23, 29};
    CpuBudgetFilter f{};
    unsigned cap = 0, minCap = 255;
    double maxT = 0, pp0Sum = 0;
    for (int i = 0; i < 480; ++i) {
        p.Step(cap, 950);
        CpuBudgetSample s{true, true, uint32_t(p.pp0W * 1000), p.instantC};
        CpuBudgetDecide(cfg, lim, f, cap, s);
        if (cap && cap < minCap) minCap = cap;
        if (i >= 240) { pp0Sum += p.pp0W; if (p.tempC > maxT) maxT = p.tempC; }
    }
    printf("pp0 budget @950: cap %u (min %u), pp0 avg %.2f W, pkg max %.0f C\n", cap, minCap, pp0Sum / 240, maxT);
    assert(minCap <= 16 && maxT < 75);             // starved while cool: the failure reproduces
    // Balance law on the same coupled plant.
    const auto l = Dell();
    Plant q{};
    q.Settle(9.0);
    q.bursts = false;
    BalanceState st{};
    st.Reset(l);
    const auto r = RunBalance(q, st, BalanceConfig{}, l, 120, 60);
    printf("balance on the same plant: cap min %u, gpu %u, pp0 %.2f W, %.1f fps\n", r.minCap, st.gpuMhz, q.pp0W, r.Fps());
    assert(r.minCap == l.capTop && st.gpuMhz == 950 && q.pp0W > 3.5);
}

// Released turbo bursts heat the package in a second; temperature-only control
// reacted too late. Under sustained load the law caps immediately and never
// releases, so the bursts and the trips go away.
void TestNoTurboUnderLoad()
{
    const auto l = Dell();
    Plant p{};
    p.rSlow = 2.2;
    p.rFast = 4.0;
    p.Settle(9.0);
    BalanceState st{};
    st.Reset(l);
    const auto r = RunBalance(p, st, BalanceConfig{}, l, 300, 5);
    assert(st.releases == 0 && st.engages == 1 && r.trips == 0);
    Plant d{};
    d.rSlow = 2.2;
    d.rFast = 4.0;
    d.Settle(9.0);
    const auto base = RunDefault(d, 300, 5);
    printf("bursting plant: balance instant max %.0f, default instant max %.0f (trips %llu)\n", r.maxInstant,
           base.maxInstant, (unsigned long long)base.trips);
    assert(r.maxInstant + 5 < base.maxInstant);
}

// A slow drift in ambient (e.g. new paste or a warm room) moves the operating
// point, not the target: the law adapts from measurements.
void TestAdaptsToCooling()
{
    const auto l = Dell();
    Plant p{};
    p.rSlow = 3.0;
    p.rFast = 4.0;
    p.Settle(7.7);
    BalanceState st{};
    st.Reset(l);
    RunBalance(p, st, BalanceConfig{}, l, 300);
    const auto hotGpu = st.gpuMhz;
    p.rSlow = 1.2; // re-pasted
    p.rFast = 2.5;
    const auto r = RunBalance(p, st, BalanceConfig{}, l, 300, 200);
    assert(st.gpuMhz == 950 && st.gpuMhz > hotGpu && r.trips == 0);
    // And a lower target is honoured on the original plant.
    Plant q{};
    q.Settle(7.7);
    BalanceState lo{};
    lo.Reset(l);
    const auto low = RunBalance(q, lo, BalanceConfig{64, 13000}, l, 400, 200);
    assert(low.maxFiltered <= 64 + 1);
}

// Light load: a single turbo-burst spike caps turbo at once; re-capping soon
// after a release backs off the next release (int88 soak: 20 releases, peaks 82-87 C).
void TestLightSpikeAndRecap()
{
    const auto l = Dell();
    const BalanceConfig cfg{};
    BalanceState st{};
    st.Reset(l);
    uint64_t now = 0;
    for (int i = 0; i < 8; ++i) BalanceDecide(cfg, l, st, Sample(62, 300, 7000, 200, 20), now += 250);
    assert(st.mode == BalanceMode::Light && st.cap == 0);
    assert(BalanceDecide(cfg, l, st, Sample(cfg.targetC + BalanceLaw::SpikeC, 300, 9000, 200, 30), now += 250) ==
           BalanceReason::Spike);
    assert(st.cap == l.capTop && st.spikes == 1 && st.lastAction == BalanceAction::CpuCapHot);
    unsigned n = 0;
    while (st.cap && n < 200) { BalanceDecide(cfg, l, st, Sample(62, 300, 7000, 200, 20), now += 250); ++n; }
    assert(st.cap == 0 && st.releases == 1 && n >= BalanceLaw::ReleaseSamples);
    // Spike again 5 s after the release: recap, and the next release waits twice as long.
    for (int i = 0; i < 20; ++i) BalanceDecide(cfg, l, st, Sample(62, 300, 7000, 200, 20), now += 250);
    BalanceDecide(cfg, l, st, Sample(cfg.targetC + BalanceLaw::SpikeC, 300, 9000, 200, 30), now += 250);
    assert(st.cap == l.capTop && st.recaps == 1 && st.releaseHold == 2 * BalanceLaw::ReleaseSamples);
    n = 0;
    while (st.cap && n < 400) { BalanceDecide(cfg, l, st, Sample(62, 300, 7000, 200, 20), now += 250); ++n; }
    assert(st.cap == 0 && n >= 2 * BalanceLaw::ReleaseSamples);
    // Calm for a minute: the release hold halves back.
    for (int i = 0; i < 300; ++i) BalanceDecide(cfg, l, st, Sample(62, 300, 7000, 200, 20), now += 250);
    assert(st.releaseHold == BalanceLaw::ReleaseSamples);
}

// Light-load heat churn (8 s: 2 s hot, 6 s cool): releases are damped.
void TestLightChurnDamped()
{
    const auto l = Dell();
    const BalanceConfig cfg{};
    BalanceState st{};
    st.Reset(l);
    uint64_t now = 0;
    for (int cycle = 0; cycle < 20; ++cycle) {
        for (int i = 0; i < 8; ++i) BalanceDecide(cfg, l, st, Sample(78, 300, 9000, 200, 25), now += 250);
        for (int i = 0; i < 24; ++i) BalanceDecide(cfg, l, st, Sample(60, 300, 7000, 200, 20), now += 250);
    }
    printf("light churn: %llu releases, %llu recaps in 160 s, release hold %u ms\n",
           (unsigned long long)st.releases, (unsigned long long)st.recaps, st.releaseHold * 250);
    assert(st.mode == BalanceMode::Light);
    assert(st.releases <= 8 && st.releaseHold > BalanceLaw::ReleaseSamples); // 20 cycles: one release each undamped
}

// A hot GPU down step leaves a ceiling: raises return at most to the level
// that was too hot for a minute (the soak's 600 <-> 950 limit cycles).
void TestGpuCeiling()
{
    const auto l = Dell();
    const BalanceConfig cfg{};
    BalanceState st{};
    st.Reset(l);
    uint64_t now = 0;
    for (int i = 0; i < 100; ++i) BalanceDecide(cfg, l, st, Sample(60), now += 250);
    assert(st.gpuMhz == 950 && st.cap == 23);
    // Hot: CPU to the floor, then the GPU down a few bins.
    for (int i = 0; i < 400 && st.gpuMhz > 800; ++i) BalanceDecide(cfg, l, st, Sample(cfg.targetC), now += 250);
    assert(st.cap == l.floorRatio && st.gpuMhz == 800 && st.gpuCeilingMhz == 850);
    // Cool again: the GPU climbs back to the ceiling only, while it lasts.
    const uint64_t hotAt = st.lastHotGpuMs;
    uint32_t top = 0;
    while (now - hotAt < BalanceLaw::CeilingMs - 1000) {
        BalanceDecide(cfg, l, st, Sample(60), now += 250);
        if (st.gpuMhz > top) top = st.gpuMhz;
    }
    assert(top == 850 && st.ceilingHolds > 0);
    for (int i = 0; i < 400; ++i) BalanceDecide(cfg, l, st, Sample(60), now += 250);
    assert(st.gpuCeilingMhz == 0 && st.gpuMhz == 950);
}

// Damping: once a down step is acting (fast filter under the target and
// falling), the slow filter's lag alone does not stack more steps.
void TestFallingHold()
{
    const auto l = Dell();
    const BalanceConfig cfg{};
    BalanceState st{};
    st.Reset(l);
    uint64_t now = 0;
    for (int i = 0; i < 100; ++i) BalanceDecide(cfg, l, st, Sample(60), now += 250);
    for (int i = 0; i < 200 && st.lastDir >= 0; ++i) BalanceDecide(cfg, l, st, Sample(cfg.targetC + 2), now += 250);
    for (int i = 0; i < 40; ++i) BalanceDecide(cfg, l, st, Sample(cfg.targetC + 2), now += 250);
    assert(st.filter.Slow() >= cfg.targetC);
    const auto downs = st.cpuDown + st.gpuDown;
    // The step worked: the package drops to 68 C; the slow filter stays >= target - 1 for a while.
    unsigned laggingSamples = 0;
    for (int i = 0; i < 16; ++i) {
        BalanceDecide(cfg, l, st, Sample(68), now += 250);
        if (st.filter.Slow() >= cfg.targetC - BalanceLaw::DownBandC) ++laggingSamples;
    }
    assert(laggingSamples >= 4 && st.fallingHolds > 0);
    assert(st.cpuDown + st.gpuDown == downs);
}

// Idle floor: 5 s of near-zero load caps the CPU at idleRatio and the render
// clock at gpuIdleMhz; one loaded sample restores both.
void TestIdleFloor()
{
    const auto l = Dell();
    const BalanceConfig cfg{};
    BalanceState st{};
    st.Reset(l);
    uint64_t now = 0;
    for (int i = 0; i < int(BalanceLaw::IdleSamples) + 8 && !st.idle; ++i)
        BalanceDecide(cfg, l, st, Sample(55, 20, 3000, 10, 3), now += 250);
    assert(st.idle && st.cap == l.idleRatio && st.GpuOverlay(l) == l.gpuIdleMhz && st.idleEntries == 1);
    assert(st.samples >= BalanceLaw::IdleSamples);
    for (int i = 0; i < 40; ++i) BalanceDecide(cfg, l, st, Sample(55, 20, 3000, 10, 8), now += 250);
    assert(st.idle && st.releases == 0);           // stays idle; no turbo release churn
    // A small burst under the wake thresholds keeps it; one at them ends it.
    BalanceDecide(cfg, l, st, Sample(55, 200, 3000, 10, 20), now += 250);
    assert(st.idle);
    assert(BalanceDecide(cfg, l, st, Sample(55, 400, 4000, 50, 10), now += 250) == BalanceReason::Wake);
    assert(!st.idle && st.cap == 0 && st.GpuOverlay(l) == 0 && st.wakes == 1);
    // Heavy load straight from idle: wake on the first sample, engage once the
    // filtered load has been heavy for 1 s (~3 s here).
    for (int i = 0; i < int(BalanceLaw::IdleSamples) + 8 && !st.idle; ++i)
        BalanceDecide(cfg, l, st, Sample(55, 20, 3000, 10, 3), now += 250);
    assert(st.idle);
    BalanceDecide(cfg, l, st, Sample(60, 950, 9000, 400, 40), now += 250);
    assert(!st.idle && st.cap == 0);
    for (int i = 0; i < 16; ++i) BalanceDecide(cfg, l, st, Sample(60, 950, 9000, 400, 40), now += 250);
    assert(st.mode == BalanceMode::Heavy && st.cap == l.capTop && st.engages == 1);
    // Hot idle wakes to capTop, not turbo.
    BalanceState h{};
    h.Reset(l);
    now = 0;
    for (int i = 0; i < 60 && !h.idle; ++i) BalanceDecide(cfg, l, h, Sample(55, 20, 3000, 10, 3), now += 250);
    for (int i = 0; i < 40; ++i) BalanceDecide(cfg, l, h, Sample(80, 20, 3000, 10, 3), now += 250);
    assert(h.idle && h.cap == l.idleRatio);
    BalanceDecide(cfg, l, h, Sample(80, 400, 4000, 50, 40), now += 250);
    assert(!h.idle && h.cap == l.capTop);
    // Without an RPn the idle floor leaves the render clock alone.
    const auto noRpn = BalanceMakeLimits(8, 23, BdwClockBootMhz, 950);
    BalanceState q{};
    q.Reset(noRpn);
    now = 0;
    for (int i = 0; i < 60 && !q.idle; ++i) BalanceDecide(cfg, noRpn, q, Sample(55, 20, 3000, 10, 3), now += 250);
    assert(q.idle && q.GpuOverlay(noRpn) == 0);
}

// Default-on arming: retry while armed and eligible, every RetryMs at most.
void TestAutoArm()
{
    BalanceAuto a{};
    assert(!a.Due(false, true, 1000));             // not armed (opt-out or `off`)
    a.armed = true;
    assert(!a.Due(false, false, 1000));            // battery / ineligible
    assert(a.Due(false, true, 1000));
    a.Note(0, 1000);
    assert(a.enables == 1 && !a.Due(true, true, 9000)); // running
    assert(!a.Due(false, true, 1000 + BalanceAuto::RetryMs - 1)); // stopped (AC lost): rate-limited
    assert(a.Due(false, true, 1000 + BalanceAuto::RetryMs));
    a.Note(-16, 7000);
    assert(a.failures == 1 && a.lastResult == -16 && !a.Due(false, true, 8000));
    a.armed = false;
    assert(!a.Due(false, true, 100000));
}

} // namespace

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestLimitsAndMeter();
    TestOverlay();
    TestExternalOwner();
    TestRampAndRelease();
    TestFlappingLoad();
    TestHotPlantSettles();
    TestPriorityBelowFloor();
    TestSpikeFastDown();
    TestBackOffNearTarget();
    TestPowerLimit();
    TestGpuIdleGivesBack();
    TestLightHot();
    TestPp0CouplingFailureMode();
    TestNoTurboUnderLoad();
    TestAdaptsToCooling();
    TestLightSpikeAndRecap();
    TestLightChurnDamped();
    TestGpuCeiling();
    TestFallingHold();
    TestIdleFloor();
    TestAutoArm();
    puts("test_thermal_balance: ok");
    return 0;
}
