#pragma once

#include <Uefi.h>
#include "boot_protocol/boot_protocol.h"

namespace brook
{
namespace bootloader
{

// Kernel entry point function type. Called with SysV ABI (arg in RDI).
using KernelEntryFn = void (__attribute__((sysv_abi)) *)(brook::BootProtocol* bootProtocol);

// Load a kernel ELF into memory.
//
// Returns the VIRTUAL entry point (from the ELF e_entry field directly).
// Sets outPhysBase to the physical address where the kernel was loaded.
//
// The virtual entry is only valid to call AFTER page tables are loaded.
KernelEntryFn LoadKernelElf(
    EFI_BOOT_SERVICES*    bootServices,
    const uint8_t*        elfData,
    UINTN                 elfSize,
    EFI_PHYSICAL_ADDRESS& outPhysBase);

} // namespace bootloader
} // namespace brook
