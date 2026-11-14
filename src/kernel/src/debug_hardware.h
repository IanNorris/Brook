#pragma once
#include <stdint.h>
namespace brook {
bool DebugHardwareHandleCommand(const char* command,uint32_t length);
// Reads /etc/hardware-payload-id (performs VFS I/O; not for render paths).
// Returns false and an empty string when absent or malformed.
bool HardwarePayloadIdRead(char* id,uint32_t capacity);
// Runs the existing one-shot, CPU-0 bootstrap on explicit first-use opt-in.
// A failed or uncertain attempt is never automatically replayed in this boot.
bool DebugHardwareAutoInitializeIntel();
}
