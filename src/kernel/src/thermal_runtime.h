#pragma once
// Kernel thermal/power sampler API (implementation: thermal_sampler.cpp).
#include <stdint.h>
#include "thermal_sampler.h"
#include "thermal_balance.h"

namespace brook {

// Starts one pinned sampling thread per CPU when the platform has the
// digital thermal sensor MSRs (bare metal Intel family 6). No-op otherwise.
void ThermalSamplerStart();
// Start an AC-only timed Broadwell-U EPB experiment, off until explicitly armed.
int ThermalStartEpbTrial(unsigned bias,unsigned seconds);
// Same admission; caps IA32_PERF_CTL to RATIO (min..max non-turbo) for SECONDS.
int ThermalStartRatioTrial(unsigned ratio,unsigned seconds);
// Same admission; closed-loop PERF_CTL cap keeping RAPL PP0 under coresMw
// and/or the package under tempC (0 = that target off). Starts when off,
// retargets when running; both zero turns it off. See cpu_power_budget.h.
int ThermalSetCpuBudget(uint32_t coresMw,int32_t tempC);

// Combined CPU + GPU balance controller (thermal_balance.h), off by default.
// Enable: same AC-only Broadwell-U admission as the budget (-95 otherwise),
// takes the PERF_CTL owner in external mode (-16 if the budget or ratio trial
// holds it). Disable, AC loss or ineligibility restore PERF_CTL on every CPU
// and remove the render clock overlay (back to the fixed/auto request).
enum class ThermalBalanceOutcome : uint8_t { None, Running, Disabled, Ineligible, OwnerLost, Unsupported };
struct ThermalBalanceStatus {
    bool available = false, enabled = false, ownerActive = false;
    ThermalBalanceOutcome outcome = ThermalBalanceOutcome::None;
    BalanceConfig config{};
    BalanceLimits limits{};
    BalanceState state{};
    BalanceSample last{};
    uint32_t appliedCap = 0, appliedGpuMhz = 0, gpuActualMhz = 0;
    uint64_t startedAt = 0, lastSampleAt = 0;
};
int ThermalBalanceEnable();
void ThermalBalanceDisable();
// Retargets live when on; kept for the next enable when off. -22 invalid.
int ThermalBalanceConfigure(const BalanceConfig& config);
ThermalBalanceStatus ThermalBalanceRead();

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
