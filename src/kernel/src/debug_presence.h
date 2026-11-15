#pragma once
#include <stdint.h>
#include "bridge_presence.h"

namespace brook {
// Kernel owner of the debug-bridge agent presence (see bridge_presence.h).
// The TCP debug listener brackets each framed request; the compositor samples
// a snapshot at most once per second for the taskbar indicator.
void DebugPresenceRequestBegin(uint32_t peer);
void DebugPresenceRequestEnd();
BridgePresenceView DebugPresenceSnapshot(uint64_t now);
// "bridge presence", "bridge presence beat SECONDS", "bridge presence detach".
bool DebugPresenceHandleCommand(const char* command);
}
