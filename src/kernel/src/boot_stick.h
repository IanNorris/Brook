#pragma once
#include <stdint.h>
namespace brook {
// After the initrd is mounted: read /etc/boot-stick-update and the loader's
// boot volume handoff; logs the identity. Grants nothing by itself.
void BootStickConfigure();
// One status-block line (only when the marker is present).
void BootStickStatusLine();
// usbN index of the disk holding the loader's boot partition, or -1.
struct Device;
Device* BootStickPartition(int* refusal=nullptr);
// Matches xHCI storage names, usb0 through usb99.
inline unsigned BootStickUsbDiskName(unsigned index,char* name) {
    name[0]='u';name[1]='s';name[2]='b';
    unsigned end=3;
    if(index>=10)name[end++]=char('0'+index/10);
    name[end++]=char('0'+index%10);
    name[end]='\0';
    return end;
}
int BootStickDiskIndex();
// Read-only read of a small file in the loader volume root (arms RO, reads, disarms). 0 = read.
int BootStickReadRootFile(const char* name, char* buf, uint32_t cap, uint32_t* len);
// BROOK_IOCTL_BOOT_STICK_*; caller has checked CAP_SYS_ADMIN.
int BootStickIoctl(uint32_t cmd, uint64_t arg, bool (*copyOut)(uint64_t, const void*, uint64_t),
                   bool (*copyIn)(void*, uint64_t, uint64_t));
}
