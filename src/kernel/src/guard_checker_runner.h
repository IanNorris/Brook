#pragma once
namespace brook {
struct Device;
// Check-only boot: never grants mount or write authority.
bool GuardCheckerStart(Device* device);
}
