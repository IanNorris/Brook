#pragma once
#include <Uefi.h>
#include "boot_protocol/boot_protocol.h"

namespace brook
{
namespace bootloader
{

// Allocate pages from UEFI memory, halting on failure.
// pageType should be an EFI_MEMORY_TYPE — use EfiLoaderData for most allocations.
EFI_PHYSICAL_ADDRESS AllocatePages(EFI_BOOT_SERVICES* bootServices, EFI_MEMORY_TYPE pageType, UINT64 sizeInBytes);

void FreePages(EFI_BOOT_SERVICES* bootServices, EFI_PHYSICAL_ADDRESS address, UINT64 sizeInBytes);

// Query the UEFI memory map and convert it to our MemoryDescriptor format.
// Allocates the output array using EfiLoaderData pages (included in the map as BootData).
// Call this immediately before ExitBootServices. Returns the map key needed for ExitBootServices.
// outMap and outCount are set on success.
UINTN BuildMemoryMap(
    EFI_BOOT_SERVICES*         bootServices,
    brook::MemoryDescriptor**  outMap,
    UINT32*                    outCount);

} // namespace bootloader
} // namespace brook
