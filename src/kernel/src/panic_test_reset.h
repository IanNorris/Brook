#pragma once
namespace brook {
// The QEMU selector is test-only. Automatic recovery may separately arm the
// firmware reset after its pending repair transaction is durably verified.
void PanicTestResetConfigure();
bool PanicTestResetEnabled();
bool PanicTestHeadlessEnabled();
void PanicTestResetAfterDump();
#if defined(BROOK_SSD_AUTO_RECOVERY)
bool PanicRecoveryResetArm();
bool PanicRecoveryResetArmed();
#endif
}
