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

// Get the number of online CPUs.
uint32_t SmpGetCpuCount();

// Get the CpuInfo for a CPU by index (0 = BSP).
const CpuInfo* SmpGetCpu(uint32_t index);

// Get the current CPU's index (based on LAPIC ID).
uint32_t SmpCurrentCpuIndex();

} // namespace brook
