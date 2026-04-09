#pragma once

#include <stdint.h>

namespace brook {

// Tag describing the purpose/owner-subsystem of a physical page or virtual
// allocation. Stored as one byte per page in the PMM tracking arrays.
enum class MemTag : uint8_t
{
    Free        = 0,    // Not allocated (default / untracked)
    KernelCode,         // Kernel .text / .rodata
    KernelData,         // Kernel .data / .bss / general kernel allocations
    KernelStack,        // Kernel stack (future: per-CPU stacks)
    PageTable,          // Intermediate page table page (PMM/VMM internal)
    Heap,               // Kernel heap (kmalloc-managed)
    Device,             // MMIO mapping
    User,               // User-space page (owned by a process)
    BootData,           // Bootloader data to be freed after boot
    Reserved,           // Out-of-range / hardware reserved
};

// PID used for all kernel-owned allocations.
static constexpr uint16_t KernelPid = 0;

} // namespace brook
