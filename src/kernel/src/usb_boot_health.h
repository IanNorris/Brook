#pragma once
#include <stdint.h>

namespace brook {
struct UsbBootResetReport {
    uint16_t vendor, device;
    uint32_t phase, error, command, status, statusNow;
    uint64_t elapsedMs;
    bool statusNowValid;
};

// The first reset outcome remains in kernel memory even if module init fails
// and its code is unloaded. Reads before publication report unavailable.
extern "C" void UsbBootReportReset(uint16_t vendor, uint16_t device,
    uint32_t phase, uint32_t error, uint32_t command, uint32_t status,
    uint32_t statusNow, bool statusNowValid, uint64_t elapsedMs);
bool UsbBootReadReset(UsbBootResetReport& report);
}
