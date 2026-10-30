#pragma once
#include <stdint.h>
namespace brook {
struct Device;
struct Vnode;
// Create an unpublished fixed-size device with512-byte geometry and byte I/O
// from a private RW FAT vnode.
// Success retains the source; the caller still drops its own vnode reference.
// Register/bind once during boot. Published devices/backings live until reboot.
Device* FatImageDeviceCreate(Vnode* source,uint64_t bytes,const char* name,int* error);
// Only before registration/binding, with no concurrent I/O. Error leaves the
// device/backing pinned. Never destroy a device exposed to a filesystem.
int FatImageDeviceDestroyUnregistered(Device* device);
}
