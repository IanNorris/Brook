#pragma once
#include <Uefi.h>

namespace brook { namespace bootloader {
// One-time inventory build only. Failure must not prevent the ordinary boot.
void SaveBootVariableInventory(EFI_HANDLE image, EFI_SYSTEM_TABLE* system);
}}
