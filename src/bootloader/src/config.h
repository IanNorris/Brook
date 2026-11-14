#pragma once
#include <Uefi.h>
#include <stdint.h>

namespace brook {
namespace bootloader {

struct BootConfig {
    char16_t target[256];   // ELF path, default L"KERNEL\\BROOK.ELF"
    uint32_t displayWidth;  // preferred GOP bounds; both must be set
    uint32_t displayHeight;
    bool updateSlots;      // opt-in paired A/B maintenance payloads
    bool recoveryMenu;     // opt-in F5 selection of an SSD recovery pair
    uint32_t recoveryMenuMs; // RECOVERY_MENU_MS: F5 wait length, default 500
    bool debugText;
    bool logMemory;
    bool logInterrupts;
};

constexpr uint32_t DefaultRecoveryMenuMs = 500;
constexpr uint32_t MaxRecoveryMenuMs     = 10000;
// Decimal milliseconds; empty, malformed or out-of-range values select the default.
inline uint32_t ParseRecoveryMenuMs(const uint8_t* val, UINTN len)
{
    if (!len || len > 5) return DefaultRecoveryMenuMs;
    uint32_t result = 0;
    for (UINTN i = 0; i < len; ++i) {
        if (val[i] < '0' || val[i] > '9') return DefaultRecoveryMenuMs;
        result = result * 10 + (val[i] - '0');
    }
    return result <= MaxRecoveryMenuMs ? result : DefaultRecoveryMenuMs;
}

// Global boot configuration, populated by LoadConfig().
extern BootConfig g_bootConfig;

// Read and parse \BROOK.CFG from the ESP. Sets defaults first;
// if the file is missing, returns silently with defaults intact.
void LoadConfig(EFI_HANDLE imageHandle, EFI_BOOT_SERVICES* bootServices);

} // namespace bootloader
} // namespace brook
