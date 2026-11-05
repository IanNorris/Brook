#pragma once
#include <stdint.h>
namespace brook {
struct Device;
// Check-only boot: never grants mount or write authority.
bool GuardCheckerStart(Device* device);
#if defined(BROOK_SSD_AUTO_RECOVERY)
struct GuardPanicSnapshot { const uint8_t* data;uint64_t sequence;uint32_t bytes,crc; };
bool GuardRecoveryPanicSnapshot(GuardPanicSnapshot& out);
#endif
}
