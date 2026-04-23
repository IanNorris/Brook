#pragma once

#include <stdint.h>

namespace brook {

// Maximum number of CPUs supported.
static constexpr uint32_t MAX_CPUS = 64;

// Per-CPU state (indexed by LAPIC ID or sequential CPU index).
struct CpuInfo {
    uint8_t  apicId;         // LAPIC ID from MADT
    bool     isBsp;          // true for the Bootstrap Processor
    bool     online;         // true after AP has signalled it's running
    uint64_t kernelStack;    // top of per-CPU kernel stack (for AP boot)
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
