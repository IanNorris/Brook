#pragma once
namespace brook {
// Framed debug command context only; worker output never uses the serial log.
bool DebugJobHandleCommand(const char* command);
}
