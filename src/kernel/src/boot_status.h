#pragma once
#include <stdint.h>

namespace brook {
// Opt-in (/etc/boot-status) end-of-boot status block for smoke payloads:
// the xHCI driver's controller/port/device text, every NIC with MAC and DHCP
// state, the bridge endpoint and BRIDGE_V3_LOCAL lines. It is printed after
// boot settles and again whenever USB or network state changes, so the last
// screen always carries it. The same marker enables background DHCP retries
// that never give up, for NICs that are late, hotplugged or behind a hub.
void BootStatusConfigure();          // before modules load (reads the marker)
bool BootStatusEnabled();
// After SchedulerInit: start queued module workers and the status thread.
void BootStatusStartThreads();
}

// Module-facing (exported) entry points.
// Replace the USB section text (multi-line, '\n' separated, bounded).
extern "C" void UsbStatusPublish(const char* text, uint32_t length);
// A live text source (the xHCI module after successful init); it is called
// at print time so PORTSC values are current. Published text is the fallback.
extern "C" void UsbStatusSetProvider(uint32_t (*provider)(char* out, uint32_t capacity));
// Topology changed: the status thread reprints the block (debounced).
extern "C" void UsbStatusChanged();
// Print the whole block now (no-op unless enabled). Reason is a short tag.
extern "C" void BootStatusPrint(const char* reason);
// Run fn(arg) on its own kernel thread once the scheduler runs (immediately
// if it already does). Returns false when the queue is full.
extern "C" bool KernelWorkerStart(const char* name, void (*fn)(void*), void* arg);
