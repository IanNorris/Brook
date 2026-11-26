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
BootGuardSlot g_selectedSlot{};
BootStick g_bootStick{};
bool g_bootStickPresent = false;
BootTimingSummary g_summary{};
bool g_ready = false;

struct PointerStamp {
    uint32_t state; // 0 empty, 1 recording, 2 ready, 3 printed
    uint32_t detail;
    uint64_t tsc, tick;
};
PointerStamp g_pointerStamps[static_cast<unsigned>(BootPointerEvent::Count)];

void PublishBootLine(const char* line)
{
    SerialPuts(line);
    DebugOverlayPuts(line);
}

inline uint64_t ReadTsc()
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}
}

void BootTimingPointerMark(BootPointerEvent event, uint32_t detail)
{
    auto& stamp = g_pointerStamps[static_cast<unsigned>(event)];
    if (__atomic_load_n(&stamp.state, __ATOMIC_RELAXED)) return;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&stamp.state, &expected, 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return;
    stamp.tsc = ReadTsc();
    stamp.tick = ApicTickCount();
    stamp.detail = detail;
    __atomic_store_n(&stamp.state, 2, __ATOMIC_RELEASE);
}

static void PointerClock(BootText& text, uint64_t tsc, uint64_t tick)
{
    text.Add(" kernel_tsc="); text.Number(tsc);
    text.Add(" tick_ms="); text.Number(tick);
    const uint64_t khz = ApicGetTscKhz();
    if (khz && g_kernelEntryTsc && tsc >= g_kernelEntryTsc) {
        text.Add(" kernel_ms="); text.Number((tsc - g_kernelEntryTsc) / khz);
    }
}

void BootTimingPointerFlush()
{
    static const char* const names[] = {"ingress", "wm", "seat_bound", "wayland_delivered", "cursor", "desktop"};
    static_assert(sizeof(names) / sizeof(*names) == static_cast<unsigned>(BootPointerEvent::Count));
    for (unsigned i = 0; i < static_cast<unsigned>(BootPointerEvent::Count); ++i) {
        auto& stamp = g_pointerStamps[i];
        if (__atomic_load_n(&stamp.state, __ATOMIC_ACQUIRE) != 2) continue;
        char line[176];
        BootText text(line, sizeof(line));
        text.Add("BOOT_POINTER "); text.Add(names[i]);
        PointerClock(text, stamp.tsc, stamp.tick);
        if (i == static_cast<unsigned>(BootPointerEvent::Cursor)) {
            static const char* const reasons[] = {"first_frame", "device_available", "visibility_restored", "pixels_ready"};
            text.Add(" trigger="); text.Add(reasons[stamp.detail & 3]);
            text.Add(" route="); text.Add(stamp.detail & 4 ? "renderer" : "compositor");
        } else {
            text.Add(" detail="); text.Number(stamp.detail);
        }
        text.Add("\n");
        PublishBootLine(line);
        __atomic_store_n(&stamp.state, 3, __ATOMIC_RELEASE);
    }
}

extern "C" void BootTimingUsbPointer(bool bound, uint32_t controller,
                                     const char* path, uint16_t vendor, uint16_t product)
{
    char line[176];
    BootText text(line, sizeof(line));
    text.Add(bound ? "BOOT_POINTER usb_bound" : "BOOT_POINTER usb_enumerated");
    PointerClock(text, ReadTsc(), ApicTickCount());
    text.Add(" controller="); text.Number(controller);
    text.Add(" port="); text.Add(path);
    text.Add(" vid="); text.Number(vendor);
    text.Add(" pid="); text.Number(product);
    text.Add("\n");
    PublishBootLine(line);
}

void BootTimingCaptureHandoff(const BootProtocol* protocol)
{
    g_inputs.kernelEntryTsc = g_kernelEntryTsc;
    if (!protocol) return;
    g_inputs.protocolVersion = protocol->version;
    // Older loaders end the handoff before `timing` and leave `extensions`
    // zero; never read past the version-5 fields unless the marker says so.
    g_inputs.loaderTimingPresent = BootHasTiming(protocol);
    if (g_inputs.loaderTimingPresent) g_inputs.loader = protocol->timing;
    if (BootHasGuardSlot(protocol)) g_selectedSlot = protocol->guardSlot;
    if (BootHasBootStick(protocol)) { g_bootStick = protocol->bootStick; g_bootStickPresent = true; }
}

bool BootTimingHandoffStick(BootStick* out) { if (g_bootStickPresent && out) *out = g_bootStick; return g_bootStickPresent; }

uint32_t BootTimingHandoffFlags() { return g_inputs.loader.flags; }
BootGuardSlot BootTimingHandoffSlot() { return g_selectedSlot; }

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
    BootTimingPointerMark(BootPointerEvent::Desktop);
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
