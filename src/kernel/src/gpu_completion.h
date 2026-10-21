#pragma once
#include <stdint.h>
namespace brook {
// Boot-only, single-device registration. The caller promises that the MMIO and
// virtqueue pages remain mapped/owned and the device is not reset until reboot.
// EVENT_IDX must not be negotiated. Replacements are rejected. Provider module
// pages are permanently retained before installing the built-in IRQ callback.
extern "C" bool GpuCompletionRegister(const void* provider, volatile uint8_t* isr,
    volatile uint16_t* used, volatile uint16_t* availFlags, uint8_t irq, bool eventIdx);
// Optional pause within the existing completion loop. Never consumes a used
// entry. False means use the caller's bounded polling fallback. Deadline comes
// from the original command publication and is never renewed by a wake.
extern "C" bool GpuCompletionEnabled();
extern "C" bool GpuCompletionPause(uint16_t expectedUsed, uint64_t startNs, uint64_t deadlineNs);
// Read once by the module during serialized boot initialization. These private
// experiments are off unless an exact named fw_cfg file contains the byte '1'.
// 1: scoped late vertex completion recovery; 2: inject one outstanding timeout.
extern "C" bool GpuRecoveryBootOption(uint32_t option);
bool GpuCompletionControl(bool enabled);
struct GpuCompletionStats {
    bool registered, enabled, waiting, saturated;
    uint8_t irq, vector;
    uint64_t irqCalls, queueIrqs, notifications, waits, blockCalls;
    uint64_t readyBeforeBlock, incompleteResumes, safetyExpiries;
    uint64_t latencySamples, latencySumNs, latencyMaxNs, invalidLatency;
};
GpuCompletionStats GpuCompletionRead();
}
