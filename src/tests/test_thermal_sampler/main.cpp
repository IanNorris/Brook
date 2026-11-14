// Host test for the pure thermal/power sampler logic.
#include "thermal_sampler.h"
#include <cassert>
#include <cstdio>

using namespace brook;

int main() {
    // DTS: valid bit, readout below TjMax.
    assert(ThermalDecodeTjMax(uint64_t(100) << 16) == 100);
    assert(ThermalDecodeTjMax(uint64_t(105) << 16 | 0xff) == 105);
    assert(ThermalDecodeTjMax(0) == 100);
    assert(ThermalDecodeDts((1ull << 31) | (uint64_t(19) << 16), 100) == 81);
    assert(ThermalDecodeDts((1ull << 31), 100) == 100);
    assert(ThermalDecodeDts(uint64_t(19) << 16, 100) == -1);           // not valid
    assert(ThermalDecodeDts((1ull << 31) | (uint64_t(127) << 16), 100) == 0);
    assert(ThermalDecodeDts((1ull << 31), 0) == -1);

    // RAPL: ESU 14 (Broadwell default, 61 uJ units); 1 s at 11.7 W.
    const uint64_t unit = uint64_t(14) << 8;
    const uint32_t units = uint32_t(11.7 * (1u << 14));
    assert(ThermalEnergyMw(1000, 1000 + units, unit, 1000000) == 11699 ||
           ThermalEnergyMw(1000, 1000 + units, unit, 1000000) == 11700);
    // Wraparound of the 32-bit counter.
    assert(ThermalEnergyMw(0xffffff00u, 0x00000100u, unit, 1000) == uint32_t((512ull * 1000000 >> 14) * 1000 / 1000));
    assert(ThermalEnergyMw(5, 10, unit, 0) == 0);

    // Frequency and C0 residency.
    assert(ThermalMhz(3000, 2000, 2200) == 3300);
    assert(ThermalMhz(1, 0, 2200) == 0);
    assert(ThermalC0Percent(500, 1000) == 50);
    assert(ThermalC0Percent(2000, 1000) == 100);
    assert(ThermalC0Percent(1, 0) == 0);

    // Per-CPU slot packing round-trips, including a negative temperature.
    ThermalCpu c{};
    c.tempC = -1; c.mhz = 2700; c.c0Percent = 42; c.ratio = 27; c.valid = true;
    ThermalCpu d = ThermalUnpackCpu(ThermalPackCpu(c));
    assert(d.tempC == -1 && d.mhz == 2700 && d.c0Percent == 42 && d.ratio == 27 && d.valid);
    c.tempC = 88; c.valid = false;
    d = ThermalUnpackCpu(ThermalPackCpu(c));
    assert(d.tempC == 88 && !d.valid);

    // Summary ignores invalid CPUs.
    ThermalCpu cpus[4]{};
    cpus[0] = {80, 2000, 50, 20, true};
    cpus[1] = {85, 3000, 70, 30, true};
    cpus[2] = {99, 9000, 99, 90, false};
    ThermalSample s{};
    ThermalSummariseCpus(cpus, 4, s);
    assert(s.maxCoreC == 85 && s.cpuMhz == 2500 && s.c0Percent == 60);
    ThermalSample none{};
    ThermalSummariseCpus(cpus + 3, 1, none);
    assert(none.maxCoreC == -1 && none.cpuMhz == 0);

    // Ring keeps the newest N, oldest first.
    ThermalRing<4> ring;
    assert(!ring.Latest());
    for (int i = 0; i < 6; ++i) { ThermalSample v{}; v.tick = uint64_t(i); ring.Push(v); }
    assert(ring.count == 4 && ring.At(0).tick == 2 && ring.At(3).tick == 5 && ring.Latest()->tick == 5);

    // Maxima.
    ThermalMaxima m;
    ThermalSample a{}; a.pkgC = 70; a.pkgMw = 9000; a.gpuMhz = 500;
    ThermalSample b{}; b.pkgC = 99; b.pkgMw = 5000; b.gpuMhz = 950; b.maxCoreC = 97;
    m.Note(a); m.Note(b);
    assert(m.pkgC == 99 && m.pkgMw == 9000 && m.gpuMhz == 950 && m.coreC == 97);

    // Graph points: one per ThermalGraphEvery samples, max temp + mean power.
    ThermalGraphAccumulator acc;
    ThermalSample point{};
    int points = 0;
    for (uint32_t i = 0; i < ThermalGraphEvery * 3; ++i) {
        ThermalSample v{};
        v.pkgC = int16_t(70 + (i % ThermalGraphEvery));
        v.pkgMw = 10000 + 1000 * (i % ThermalGraphEvery);
        if (acc.Add(v, point)) {
            ++points;
            assert(point.pkgC == int16_t(70 + ThermalGraphEvery - 1));
            assert(point.pkgMw == 10000 + 1000 * (ThermalGraphEvery - 1) / 2);
        }
    }
    assert(points == 3);

    // Colour bands against TjMax.
    assert(ThermalColour(95, 100) == 0xe06c75);
    assert(ThermalColour(80, 100) == 0xe5bf69);
    assert(ThermalColour(60, 100) == 0x63b782);
    assert(ThermalColour(-1, 100) == 0x7f8896);

    std::puts("test_thermal_sampler: ok");
    return 0;
}
