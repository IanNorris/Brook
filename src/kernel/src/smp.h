#pragma once

#include <stdint.h>

struct KernelCpuEnv;

namespace brook {

// Maximum number of CPUs supported.
static constexpr uint32_t MAX_CPUS = 64;

// Per-CPU state (indexed by LAPIC ID or sequential CPU index).
struct CpuInfo {
    uint8_t  apicId;         // LAPIC ID from MADT
    bool     isBsp;          // true for the Bootstrap Processor
    bool     online;         // true after AP has signalled it's running
    uint64_t kernelStack;    // top of per-CPU kernel stack (for AP boot)
    volatile uint64_t currentCr3;   // current CR3 value (updated at context switch)
};

// Initialise SMP: detect CPUs from MADT, boot all APs.
// Must be called after GDT, IDT, VMM, APIC are initialised.
// Returns the number of CPUs brought online (including BSP).
uint32_t SmpInit();

// Activate APs into the scheduler. Must be called after SchedulerInit()
// and after all processes have been added. Sets up per-CPU GDT/TSS,
// KernelCpuEnv, SYSCALL MSRs, LAPIC timer, then enters scheduler loop.
void SmpPrepareAPs();
void SmpActivateAPs();

// Get the number of online CPUs.
uint32_t SmpGetCpuCount();

// Get the CpuInfo for a CPU by index (0 = BSP).
const CpuInfo* SmpGetCpu(uint32_t index);

// Get the current CPU's index (based on LAPIC ID).
uint32_t SmpCurrentCpuIndex();

// Update per-CPU CR3 tracking (called from scheduler context switch).
void SmpSetCurrentCr3(uint32_t cpuIndex, uint64_t cr3);

// Enable fast (gs-based) CPU index lookup.  Call this *after* the BSP's
// KernelCpuEnv is allocated, env->cpuIndex = 0 is written, and
// CpuSetKernelGsBase has been called — i.e. once gs:176 is meaningful.
// Before this is called, SmpCurrentCpuIndex falls back to the slower
// (and migration-racy) ApicGetId-based lookup.
void SmpEnableFastCpuIndex();

// GS-free resolution of the CURRENT CPU's apicId, index, and KernelCpuEnv*.
// Identifies the CPU via ApicGetId() (LAPIC MMIO — no gs access) and maps it
// through the MADT CpuInfo table, so it is safe to call when the GS base is
// bogus (user/0).  Used by the BRO-178 catch-at-scene guard to recover a valid
// kernel GS base before any gs-relative code runs.  Returns false (and null
// env) if the LAPIC id is not found in the table.
__attribute__((no_instrument_function))
bool SmpResolveCpuNoGs(uint8_t* apicOut, uint32_t* idxOut, KernelCpuEnv** envOut);

// Halt all application processors via NMI broadcast.
// Each AP's state (RIP, RSP, RBP) is captured in the NMI handler.
// Returns the number of APs successfully halted.
// Must be called from BSP only. Safe to call from panic/exception context.
uint32_t SmpHaltAllAPs();

// Check if a panic halt is active (set by SmpHaltAllAPs).
bool SmpIsPanicActive();

// Per-CPU halted state captured by NMI handler.
struct CpuHaltedState {
    uint64_t rip;
    uint64_t rsp;
    uint64_t rbp;
    uint16_t pid;       // PID of process running on that CPU (0 if none)
    bool     halted;    // true if this CPU has been halted
};

// Get the halted state for a CPU (valid only after SmpHaltAllAPs).
const CpuHaltedState* SmpGetHaltedState(uint32_t cpuIndex);

} // namespace brook
