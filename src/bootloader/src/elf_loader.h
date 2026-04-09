#pragma once

#include <Uefi.h>
#include "boot_protocol/boot_protocol.h"

namespace brook
{
namespace bootloader
{

// Kernel entry point signature.
// __attribute__((sysv_abi)) ensures the bootloader (compiled with MS x64 ABI)
// calls this with SysV calling convention — argument in RDI, not RCX.
using KernelEntryFn = void (__attribute__((sysv_abi)) *)(brook::BootProtocol* bootProtocol);

// Validate and load a kernel ELF image into memory.
//
// elfData:     Pointer to the raw ELF file bytes in memory.
// elfSize:     Size of the ELF file in bytes.
// bootServices: Used for page allocation.
//
// On success: allocates pages, loads all PT_LOAD segments, zeros BSS,
//             records the loaded region in bootProtocol's memory map,
//             returns the kernel entry point.
// On failure: calls Halt().
KernelEntryFn LoadKernelElf(
    EFI_BOOT_SERVICES*  bootServices,
    const uint8_t*      elfData,
    UINTN               elfSize);

} // namespace bootloader
} // namespace brook
