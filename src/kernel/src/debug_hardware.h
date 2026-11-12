#pragma once
#include <stdint.h>
namespace brook {
bool DebugHardwareHandleCommand(const char* command,uint32_t length);
// Runs the existing one-shot, CPU-0 bootstrap on explicit first-use opt-in.
// A failed or uncertain attempt is never automatically replayed in this boot.
bool DebugHardwareAutoInitializeIntel();
}
