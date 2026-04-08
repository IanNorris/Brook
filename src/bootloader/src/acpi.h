#pragma once
#include <Uefi.h>
#include "boot_protocol/boot_protocol.h"

namespace brook
{
namespace bootloader
{

// Search the UEFI configuration table for the ACPI 2.0 RSDP.
// Returns false if not found (ACPI 2.0 is required).
bool AcpiInit(EFI_SYSTEM_TABLE* systemTable, brook::AcpiInfo& outAcpi);

} // namespace bootloader
} // namespace brook
