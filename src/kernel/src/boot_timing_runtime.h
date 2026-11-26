#pragma once
#include <stdint.h>

namespace brook {
struct BootProtocol;
struct BootGuardSlot;
struct BootTimingSummary;
// Copy the loader's timing-extension stamps (if any) and the kernel-entry TSC. Call first
// in KernelMainBody, while the loader's stack-resident handoff is intact.
void BootTimingCaptureHandoff(const BootProtocol* protocol);
// Immutable after early boot, including on maintenance kernels.
uint32_t BootTimingHandoffFlags();
BootGuardSlot BootTimingHandoffSlot();
struct BootStick;
// Boot volume + file set from a STICK_SETS loader; false when absent.
bool BootTimingHandoffStick(BootStick* out);
// Read the ACPI FPDT boot record. Call after AcpiInit().
void BootTimingCaptureFirmware();
// Once, when the desktop is first presented: stamp, compute and log
// BOOT_TIMING. The fallback is the previous kernel-clock duration, used
// only when the TSC rate is unknown.
void BootTimingDesktopReady(bool fallbackValid, uint64_t fallbackMs);
// Null until BootTimingDesktopReady has run.
const BootTimingSummary* BootTimingRead();

enum class BootPointerEvent : uint8_t {
    Ingress, Wm, Seat, Wayland, Cursor, Desktop, Count
};
void BootTimingPointerMark(BootPointerEvent event, uint32_t detail = 0);
void BootTimingPointerFlush();
extern "C" void BootTimingUsbPointer(bool bound, uint32_t controller,
                                     const char* path, uint16_t vendor, uint16_t product);
}
