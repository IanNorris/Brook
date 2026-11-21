#pragma once
// CPU/GPU thermal and power sampling: pure decode, history and summary logic
// (host-testable). The kernel side (thermal_sampler.cpp) reads the MSRs from
// one pinned kernel thread per CPU every ThermalPeriodMs and publishes here.
//
// MSRs (Intel SDM vol. 4, Broadwell client 06_3DH):
//   0x19c IA32_THERM_STATUS (per core)      0x1b1 IA32_PACKAGE_THERM_STATUS
//   0x1a2 MSR_TEMPERATURE_TARGET (TjMax)    0x606 MSR_RAPL_POWER_UNIT
//   0x611 PKG / 0x639 PP0 (cores) / 0x641 PP1 (GT) energy status
//   0xe7 IA32_MPERF, 0xe8 IA32_APERF, 0xce MSR_PLATFORM_INFO (base ratio)
#include <stdint.h>

namespace brook {

constexpr uint32_t ThermalMaxCpus = 8;
constexpr uint32_t ThermalPeriodMs = 250;
constexpr uint32_t ThermalHistory = 240;      // 60 s of 250 ms samples
constexpr uint32_t ThermalGraphHistory = 120; // 2 min of 1 s points for the taskbar
constexpr uint32_t ThermalGraphEvery = 4;     // one graph point per 4 samples (1 s)

// Digital thermal sensor readout: bits 22:16 are degrees below TjMax, bit 31
// says the reading is valid. Returns -1 for an invalid reading.
inline int32_t ThermalDecodeDts(uint64_t status, uint32_t tjmax) {
    if (!(status & (1ull << 31)) || !tjmax) return -1;
    const uint32_t below = uint32_t(status >> 16) & 0x7f;
    return below > tjmax ? 0 : int32_t(tjmax - below);
}
// MSR_TEMPERATURE_TARGET bits 23:16; 100 C when the field is zero.
inline uint32_t ThermalDecodeTjMax(uint64_t target) {
    const uint32_t t = uint32_t(target >> 16) & 0xff;
    return t ? t : 100;
}
// RAPL energy counters are 32-bit and wrap; the energy status unit is
// 1/2^ESU joules (MSR_RAPL_POWER_UNIT bits 12:8). Returns milliwatts.
inline uint32_t ThermalEnergyMw(uint32_t before, uint32_t after, uint64_t unitMsr, uint64_t elapsedUs) {
    if (!elapsedUs) return 0;
    const uint32_t esu = uint32_t(unitMsr >> 8) & 0x1f;
    const uint64_t delta = uint32_t(after - before);
    const uint64_t microjoules = (delta * 1000000ull) >> esu;
    const uint64_t mw = microjoules * 1000ull / elapsedUs;
    return mw > 0xffffffffull ? 0xffffffffu : uint32_t(mw);
}
// Intel SDM Vol. 3B, Package RAPL Domain. The halves of 0x610 have
// identical limit fields. 0x606 supplies the binary power and time scales.
struct ThermalRaplUnits {
    uint32_t powerExponent, energyExponent, timeExponent;
};

inline ThermalRaplUnits ThermalDecodeRaplUnits(uint64_t unitMsr) {
    return {uint32_t(unitMsr) & 0xf, uint32_t(unitMsr >> 8) & 0x1f,
            uint32_t(unitMsr >> 16) & 0xf};
}

struct ThermalRaplLimit {
    uint64_t powerUw, timeWindowUs;
    bool enabled, clamp;
};

inline ThermalRaplLimit ThermalDecodeRaplLimit(uint32_t limit, ThermalRaplUnits units) {
    const uint32_t exponent = (limit >> 17) & 0x1f;
    const uint32_t quarterFraction = (limit >> 22) & 3;
    // Time window = 2^Y * (1 + Z/4) * 2^-TU seconds.
    return {((limit & 0x7fffu) * 1000000ull) >> units.powerExponent,
            ((250000ull * (4 + quarterFraction)) << exponent) >> units.timeExponent,
            (limit & (1u << 15)) != 0, (limit & (1u << 16)) != 0};
}

struct ThermalRaplPackageLimits {
    ThermalRaplUnits units;
    ThermalRaplLimit pl1, pl2;
    bool locked;
};

inline ThermalRaplPackageLimits ThermalDecodeRaplPackageLimits(uint64_t unitMsr, uint64_t limitMsr) {
    const auto units = ThermalDecodeRaplUnits(unitMsr);
    return {units, ThermalDecodeRaplLimit(uint32_t(limitMsr), units),
            ThermalDecodeRaplLimit(uint32_t(limitMsr >> 32), units), (limitMsr >> 63) != 0};
}

// Average frequency while in C0 = base * dAPERF / dMPERF.
inline uint32_t ThermalMhz(uint64_t daperf, uint64_t dmperf, uint32_t baseMhz) {
    if (!dmperf || !baseMhz) return 0;
    const uint64_t mhz = daperf * uint64_t(baseMhz) / dmperf;
    return mhz > 0xffff ? 0xffff : uint32_t(mhz);
}
// C0 residency: MPERF counts at the base clock only in C0; the invariant TSC
// counts at (nominally) the same rate all the time.
inline uint32_t ThermalC0Percent(uint64_t dmperf, uint64_t dtsc) {
    if (!dtsc) return 0;
    const uint64_t p = dmperf * 100ull / dtsc;
    return p > 100 ? 100 : uint32_t(p);
}

struct ThermalCpu {
    int16_t tempC = -1;   // core DTS
    uint16_t mhz = 0;     // C0 average frequency
    uint8_t c0Percent = 0;
    uint8_t ratio = 0;    // IA32_PERF_STATUS current ratio (x100 MHz)
    bool valid = false;
};
// Pack/unpack so a per-CPU slot can be published with one 64-bit atomic.
inline uint64_t ThermalPackCpu(const ThermalCpu& c) {
    return uint64_t(uint16_t(c.tempC)) | (uint64_t(c.mhz) << 16) | (uint64_t(c.c0Percent) << 32) |
           (uint64_t(c.ratio) << 40) | (uint64_t(c.valid ? 1 : 0) << 48);
}
inline ThermalCpu ThermalUnpackCpu(uint64_t v) {
    ThermalCpu c;
    c.tempC = int16_t(uint16_t(v));
    c.mhz = uint16_t(v >> 16);
    c.c0Percent = uint8_t(v >> 32);
    c.ratio = uint8_t(v >> 40);
    c.valid = (v >> 48) & 1;
    return c;
}

struct ThermalSample {
    uint64_t tick = 0;     // ApicTickCount ms
    int16_t pkgC = -1;     // package DTS
    int16_t maxCoreC = -1; // hottest core
    uint32_t pkgMw = 0, coreMw = 0, gtMw = 0;
    uint16_t cpuMhz = 0;   // mean C0 frequency over valid CPUs
    uint16_t gpuMhz = 0;   // last RPSTAT1 CAGF a render job saw (0 if none recent)
    uint8_t c0Percent = 0; // mean C0 residency over valid CPUs
};

template <uint32_t N>
struct ThermalRing {
    ThermalSample s[N];
    uint32_t next = 0, count = 0;
    void Push(const ThermalSample& v) {
        s[next] = v;
        next = (next + 1) % N;
        if (count < N) ++count;
    }
    // i = 0 is the oldest retained sample.
    const ThermalSample& At(uint32_t i) const { return s[(next + N - count + i) % N]; }
    const ThermalSample* Latest() const { return count ? &s[(next + N - 1) % N] : nullptr; }
};

struct ThermalMaxima {
    int16_t pkgC = -1, coreC = -1;
    uint32_t pkgMw = 0, gtMw = 0;
    uint16_t cpuMhz = 0, gpuMhz = 0;
    void Note(const ThermalSample& v) {
        if (v.pkgC > pkgC) pkgC = v.pkgC;
        if (v.maxCoreC > coreC) coreC = v.maxCoreC;
        if (v.pkgMw > pkgMw) pkgMw = v.pkgMw;
        if (v.gtMw > gtMw) gtMw = v.gtMw;
        if (v.cpuMhz > cpuMhz) cpuMhz = v.cpuMhz;
        if (v.gpuMhz > gpuMhz) gpuMhz = v.gpuMhz;
    }
};

// Combine per-CPU readings into one sample's CPU fields.
inline void ThermalSummariseCpus(const ThermalCpu* cpus, uint32_t n, ThermalSample& out) {
    uint32_t valid = 0, mhz = 0, c0 = 0;
    int16_t hottest = -1;
    for (uint32_t i = 0; i < n; ++i) {
        if (!cpus[i].valid) continue;
        ++valid;
        mhz += cpus[i].mhz;
        c0 += cpus[i].c0Percent;
        if (cpus[i].tempC > hottest) hottest = cpus[i].tempC;
    }
    out.maxCoreC = hottest;
    out.cpuMhz = valid ? uint16_t(mhz / valid) : 0;
    out.c0Percent = valid ? uint8_t(c0 / valid) : 0;
}

// Accumulates ThermalGraphEvery samples into one taskbar graph point (max
// temperature, mean power) so the taskbar changes at most once a second.
struct ThermalGraphAccumulator {
    uint32_t n = 0;
    int16_t pkgC = -1;
    uint64_t pkgMw = 0;
    // Returns true (and fills `point`) when a point is complete.
    bool Add(const ThermalSample& v, ThermalSample& point) {
        if (v.pkgC > pkgC) pkgC = v.pkgC;
        pkgMw += v.pkgMw;
        if (++n < ThermalGraphEvery) return false;
        point = v;
        point.pkgC = pkgC;
        point.pkgMw = uint32_t(pkgMw / n);
        n = 0; pkgC = -1; pkgMw = 0;
        return true;
    }
};

// Colour band for a package temperature against TjMax (taskbar graph/text).
inline uint32_t ThermalColour(int32_t tempC, uint32_t tjmax) {
    if (tempC < 0) return 0x7f8896;
    const int32_t margin = int32_t(tjmax) - tempC;
    return margin <= 10 ? 0xe06c75 : margin <= 25 ? 0xe5bf69 : 0x63b782;
}

} // namespace brook
