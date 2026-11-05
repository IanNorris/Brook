#pragma once
#include <stdint.h>
#include "panic_snapshot.h"
namespace brook {
struct Device;
// Check-only boot: never grants mount or write authority.
bool GuardCheckerStart(Device* device);
#if defined(BROOK_SSD_GUARDED_DESKTOP)
bool GuardedDesktopBridgeEnabled();
#endif
#if defined(BROOK_SSD_GUARD_PENDING_TEST) || defined(BROOK_SSD_GUARDED_DESKTOP)
void GuardTestPrepareDesktopHome();
#endif
}
