#pragma once
#include <stdint.h>

namespace brook {
struct BootProtocol;
struct BootTimingSummary;
// Copy the loader's v6 stamps (if any) and the kernel-entry TSC. Call first
// in KernelMainBody, while the loader's stack-resident handoff is intact.
void BootTimingCaptureHandoff(const BootProtocol* protocol);
// Read the ACPI FPDT boot record. Call after AcpiInit().
void BootTimingCaptureFirmware();
// Once, when the desktop is first presented: stamp, compute and log
// BOOT_TIMING. The fallback is the previous kernel-clock duration, used
// only when the TSC rate is unknown.
void BootTimingDesktopReady(bool fallbackValid, uint64_t fallbackMs);
// Null until BootTimingDesktopReady has run.
const BootTimingSummary* BootTimingRead();
}
