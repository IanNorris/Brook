#pragma once
#include <Uefi.h>
#include <stdint.h>

namespace brook {
namespace bootloader {

struct BootConfig {
    char16_t target[256];   // ELF path, default L"KERNEL\\BROOK.ELF"
    bool debugText;
    bool logMemory;
    bool logInterrupts;
};

// Global boot configuration, populated by LoadConfig().
extern BootConfig g_bootConfig;

// Read and parse \BROOK.CFG from the ESP. Sets defaults first;
// if the file is missing, returns silently with defaults intact.
void LoadConfig(EFI_HANDLE imageHandle, EFI_BOOT_SERVICES* bootServices);

} // namespace bootloader
} // namespace brook
