#pragma once
// Kernel thermal/power sampler API (implementation: thermal_sampler.cpp).
#include <stdint.h>
#include "thermal_sampler.h"

namespace brook {

// Starts one pinned sampling thread per CPU when the platform has the
// digital thermal sensor MSRs (bare metal Intel family 6). No-op otherwise.
void ThermalSamplerStart();
// Start an AC-only timed Broadwell-U EPB experiment, off until explicitly armed.
int ThermalStartEpbTrial(unsigned bias,unsigned seconds);
// Same admission; caps IA32_PERF_CTL to RATIO (min..max non-turbo) for SECONDS.
int ThermalStartRatioTrial(unsigned ratio,unsigned seconds);

struct ThermalSnapshot {
    bool available = false, rapl = false, aperf = false;
    uint32_t tjmax = 0, baseMhz = 0, cpuCount = 0;
    uint64_t samples = 0, misplaced = 0; // misplaced = per-CPU reads skipped off their CPU
    ThermalSample latest{};
    ThermalMaxima maxima{};
    ThermalCpu cpus[ThermalMaxCpus]{};
};
bool ThermalReadSnapshot(ThermalSnapshot& out);
// Oldest-first copies; return the number written.
uint32_t ThermalReadHistory(ThermalSample* out, uint32_t capacity);
uint32_t ThermalReadGraph(ThermalSample* out, uint32_t capacity);
void ThermalResetMaxima();

// Power-management registers for scoping (read on the calling CPU; package
// scope unless noted). `valid` is false off the supported platform.
struct ThermalPowerRegs {
    bool valid = false, epbSupported = false, hwpSupported = false, c8910 = false;
    uint32_t model = 0, cpu = 0;
    uint64_t miscEnable = 0, perfCtl = 0, perfStatus = 0, epb = 0, platformInfo = 0,
             turboRatioLimit = 0, pkgCstConfig = 0, pkgPowerLimit = 0, pkgPowerInfo = 0, raplPowerUnit = 0;
    uint32_t mwaitLeaf5Edx = 0; // CPUID.05H:EDX MWAIT sub-states per C-state
    // Residency counters (TSC-rate ticks since reset).
    uint64_t pkgC2 = 0, pkgC3 = 0, pkgC6 = 0, pkgC7 = 0, pkgC8 = 0, pkgC9 = 0, pkgC10 = 0;
    uint64_t coreC3 = 0, coreC6 = 0, coreC7 = 0, tsc = 0;
};
ThermalPowerRegs ThermalReadPowerRegs();

} // namespace brook
