#pragma once
#include <stdint.h>
namespace brook {
bool DebugHardwareHandleCommand(const char* command,uint32_t length);
// Reads /etc/hardware-payload-id (performs VFS I/O; not for render paths).
// Returns false and an empty string when absent or malformed.
bool HardwarePayloadIdRead(char* id,uint32_t capacity);
struct DesktopBuildProvenance;
bool HardwareBuildProvenanceRead(DesktopBuildProvenance& provenance);
// Runs the existing one-shot, CPU-0 bootstrap on explicit first-use opt-in.
// A failed or uncertain attempt is never automatically replayed in this boot.
bool DebugHardwareAutoInitializeIntel();
}

// A PCI NIC module's diagnostic command: "bridge hardware nic <args>" calls
// handler(args, out, capacity); the returned text goes back JSON-quoted.
// The handler runs on the debug command thread; nullptr unregisters.
extern "C" void DebugNicCommandSetHandler(uint32_t (*handler)(const char* args, char* out, uint32_t capacity));
extern "C" void DebugUsbCommandSetHandler(uint32_t (*handler)(const char* args, char* out, uint32_t capacity));
