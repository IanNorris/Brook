#pragma once
namespace brook {
bool DebugUiHandleCommand(const char* command);
// Called only by the compositor, between input/catalog publication and render.
void DebugUiPoll();
}
