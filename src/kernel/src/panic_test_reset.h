#pragma once
namespace brook {
// Disposable QEMU test path only. This is never enabled on physical hardware.
void PanicTestResetConfigure();
void PanicTestResetAfterDump();
}
