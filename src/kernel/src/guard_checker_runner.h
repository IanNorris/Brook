#pragma once
#include <stdint.h>
#include "panic_snapshot.h"
namespace brook {
struct Device;
// Check-only boot: never grants mount or write authority.
bool GuardCheckerStart(Device* device);
#if defined(BROOK_SSD_GUARDED_DESKTOP)
bool GuardedDesktopBridgeEnabled();
struct BootProtocol;
// Copy the verified loader selection while the original handoff is mapped.
void GuardCheckerCaptureBootSlot(const BootProtocol* boot);
#if defined(BROOK_SSD_AUTO_RECOVERY)
struct AcShutdownRecord;
bool GuardCheckedAcShutdownPrior(AcShutdownRecord& out);
bool GuardRecordCheckedAcShutdown(const char (&bootId)[33]);
#endif
#endif
#if defined(BROOK_SSD_GUARD_PENDING_TEST) || defined(BROOK_SSD_GUARDED_DESKTOP)
void GuardTestPrepareDesktopHome();
#endif
}
