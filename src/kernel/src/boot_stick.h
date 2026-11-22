#pragma once
#include <stdint.h>
namespace brook {
// After the initrd is mounted: read /etc/boot-stick-update and the loader's
// boot volume handoff; logs the identity. Grants nothing by itself.
void BootStickConfigure();
// One status-block line (only when the marker is present).
void BootStickStatusLine();
// BROOK_IOCTL_BOOT_STICK_*; caller has checked CAP_SYS_ADMIN.
int BootStickIoctl(uint32_t cmd, uint64_t arg, bool (*copyOut)(uint64_t, const void*, uint64_t));
}
