#include "boot_timing_runtime.h"
#include "boot_timing.h"
#include "acpi.h"
#include "apic.h"
#include "debug_overlay.h"
#include "serial.h"

// Written by KernelMain before it switches stacks (see kernel.cpp).
extern "C" uint64_t g_kernelEntryTsc;

namespace brook {
namespace {
BootTimingInputs g_inputs{};
BootTimingSummary g_summary{};
bool g_ready = false;

inline uint64_t ReadTsc()
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}
}

void BootTimingCaptureHandoff(const BootProtocol* protocol)
{
    g_inputs.kernelEntryTsc = g_kernelEntryTsc;
    if (!protocol) return;
    g_inputs.protocolVersion = protocol->version;
    // Older loaders end the handoff before `timing`; never read past it.
    if (protocol->version >= 6) g_inputs.loader = protocol->timing;
}

void BootTimingCaptureFirmware()
{
    FpdtBootRecord record{};
    if (AcpiReadBootPerformance(record)) g_inputs.fpdt = record;
    SerialPrintf("BOOT_TIMING_FPDT present=%u start_image_ms=%lu\n", unsigned(record.valid),
                 record.valid ? record.startImageStart / 1000000 : 0ul);
}

void BootTimingDesktopReady(bool fallbackValid, uint64_t fallbackMs)
{
    if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return;
    g_inputs.desktopReadyTsc = ReadTsc();
    g_inputs.tscKhz = ApicGetTscKhz();
    g_inputs.fallbackKernelValid = fallbackValid;
    g_inputs.fallbackKernelMs = fallbackMs;
    g_summary = BootTimingCompute(g_inputs);
    __atomic_store_n(&g_ready, true, __ATOMIC_RELEASE);
    // SerialPuts and the debug ring take only spinlocks; safe on the compositor thread.
    char line[160];
    BootTimingLogLine(line, sizeof(line) - 1, g_summary, g_inputs);
    size_t n = 0;while (line[n]) ++n;line[n] = '\n';line[n + 1] = 0;
    SerialPuts(line);DebugOverlayPuts(line);
    BootTimingDetailLine(line, sizeof(line) - 1, g_summary, g_inputs);
    n = 0;while (line[n]) ++n;line[n] = '\n';line[n + 1] = 0;
    SerialPuts(line);DebugOverlayPuts(line);
}

const BootTimingSummary* BootTimingRead()
{
    return __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) ? &g_summary : nullptr;
}

} // namespace brook
