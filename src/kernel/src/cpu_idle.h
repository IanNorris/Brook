#pragma once
// Runtime idle C-state selection and energy-performance bias (Broadwell-U).
// Boot default: HLT (C1), unchanged from before. `cpu idle` selects MWAIT
// states; `cpu epb` sets IA32_ENERGY_PERF_BIAS on every CPU.
#include <stdint.h>
#include "cpu_idle_policy.h"
#include "cpu_epb_trial.h"
#include "cpu_power_budget.h"

namespace brook {

constexpr uint32_t CpuIdleMaxCpus = 16;

struct CpuIdlePerCpu {
    uint64_t entries[uint32_t(CpuIdleState::Count)];
    uint64_t idleTsc;          // TSC cycles spent in HLT/MWAIT (includes the waking ISR)
    uint64_t coreC3, coreC6, coreC7; // residency MSRs, sampled by this CPU's idle loop
    uint64_t sampleTsc;        // when the residency sample was taken
    uint64_t sampleGen;        // last sample request honoured
    uint64_t epb;              // IA32_ENERGY_PERF_BIAS read back by this CPU
    uint64_t epbGen;           // last EPB request applied
    bool seen;
    // Governor (auto mode)
    CpuIdleHistory history;
    uint64_t holdUntilTsc;     // latency hold: stay at C1E until this TSC
    uint64_t autoSelected[uint32_t(CpuIdleState::Count)];
    uint64_t hits, tooDeep, tooShallow, holds;
    uint32_t lastPredictUs;
};

struct CpuIdleStatus {
    CpuIdleCaps caps;
    bool bareIntel = false, residencyMsrs = false, epbSupported = false;
    CpuIdleState requested = CpuIdleState::Hlt;
    CpuIdleChoice choice;
    uint32_t cpuCount = 0;
    CpuIdlePerCpu cpus[CpuIdleMaxCpus];
    uint64_t tsc = 0;
    uint64_t pkgC2 = 0, pkgC3 = 0, pkgC6 = 0, pkgC7 = 0, pkgCstConfig = 0;
    int32_t epbTarget = -1;     // last requested EPB, -1 = untouched
    int32_t epbFirmware = -1;   // value found at boot (-1 = unsupported)
    bool epbBootApplied = false;
    bool autoMode = false;
    uint64_t prevTsc = 0, prevPkgC2 = 0, prevPkgC3 = 0, prevPkgC6 = 0, prevPkgC7 = 0; // previous status call
    uint64_t miscEnable = 0, turboRatioLimit = 0;
};

// Called by every CPU's idle loop instead of `sti; hlt`.
void CpuIdleEnter(uint32_t cpu);
// Select a state ("hlt" restores the old behaviour). Returns the effective choice.
CpuIdleChoice CpuIdleRequest(CpuIdleState state);
// Menu-style governor: per-CPU predicted idle chooses C1E/C3/C6.
void CpuIdleRequestAuto();
// Latency-critical paths: keep the calling CPU at C1E for the next `us`.
void CpuIdleLatencyHint(uint32_t us);
// Ask every CPU's idle loop to apply EPB value (0-15); false if unsupported.
bool CpuIdleRequestEpb(uint32_t value);
// The thermal sampler owns admission and calls the trial on each pinned CPU.
int CpuIdleStartEpbTrial(uint64_t now, unsigned count, unsigned bias, uint32_t durationMs);
bool CpuIdleEpbTrialActive();
void CpuIdleTickEpbTrial(unsigned cpu, uint64_t now, bool eligible);
void CpuIdleCancelEpbTrial();
CpuEpbTrial CpuIdleReadEpbTrial();
// Same timed/restore protocol for an IA32_PERF_CTL target-ratio cap.
int CpuIdleStartRatioTrial(uint64_t now, unsigned count, unsigned ratio, uint32_t durationMs);
bool CpuIdleRatioTrialActive();
void CpuIdleTickRatioTrial(unsigned cpu, uint64_t now, bool eligible);
void CpuIdleCancelRatioTrial();
CpuEpbTrial CpuIdleReadRatioTrial();
// Closed-loop PERF_CTL power budget; shares ownership of PERF_CTL with the
// ratio trial (only one runs at a time). Set starts it when off, retargets it
// when running. -16 busy (incl. the other kind running), -22 invalid.
int CpuIdleSetBudget(uint64_t now, unsigned count, const CpuBudgetConfig& config, unsigned minRatio, unsigned maxNonTurbo);
bool CpuIdleBudgetActive();
void CpuIdleTickBudget(unsigned cpu, uint64_t now, bool eligible);
void CpuIdleBudgetControl(uint64_t now, const CpuBudgetSample& sample);
// External mode (config.external): the thermal balance controller sets the cap.
void CpuIdleBudgetSetCap(uint64_t now, unsigned cap);
// Cancels only a budget of the matching kind (built-in law vs external).
void CpuIdleCancelBudget(bool external = false);
CpuPowerBudget CpuIdleReadBudget();
// Ask idle loops to sample core residency, then read everything.
void CpuIdleRequestSample();
void CpuIdleReadStatus(CpuIdleStatus& out);
void CpuIdleResetCounters();

} // namespace brook
