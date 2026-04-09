#pragma once

#include <stdint.h>

namespace brook {

// Tag describing the purpose/owner-subsystem of a physical page or virtual
// allocation. Stored as 3 bits in PTE available bits [9-11], so exactly 8
// values (0-7) are supported.
enum class MemTag : uint8_t
{
    Free        = 0,    // Not allocated (default / untracked)
    KernelCode  = 1,    // Kernel .text / .rodata
    KernelData  = 2,    // Kernel .data / .bss / stack / general allocations
    PageTable   = 3,    // Intermediate page table page (PMM/VMM internal)
    Heap        = 4,    // Kernel heap (kmalloc-managed)
    Device      = 5,    // MMIO mapping
    User        = 6,    // User-space page (owned by a process)
    System      = 7,    // Reserved / unusable / out-of-range
};

// PID used for all kernel-owned allocations.
static constexpr uint16_t KernelPid = 0;

} // namespace brook
