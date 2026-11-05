#pragma once
#include <stdint.h>
namespace brook {
// Immutable boot-lifetime copy of a CRC-verified journal payload.
struct GuardPanicSnapshot { const uint8_t* data;uint64_t sequence;uint32_t bytes,crc; };
bool GuardRecoveryPanicSnapshot(GuardPanicSnapshot& out);
// Normal boot only: read the admitted system ESP once before user processes.
void PanicNormalSnapshotLoad();
}
