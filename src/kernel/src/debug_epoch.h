#pragma once

namespace brook {
// Immutable after first initialization in this boot. Never persist/reuse an
// old operation's token as an initialization proposal after a guest reboot.
void DebugEpochCopy(char out[33]);
bool DebugEpochMatches(const char* expected);
bool DebugEpochHandleCommand(const char* command);
}
