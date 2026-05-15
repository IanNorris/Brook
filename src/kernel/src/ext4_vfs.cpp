// ext4_vfs.cpp — Kernel-side device binding table for the ext4 module.
//
// The ext4 filesystem driver lives in a kernel module (ext4.mod).  This
// file provides just the device binding table and lookup functions that
// both the kernel (ProbeAndMountDevice) and the module (ext4_fs_mount)
// can access via the ksymtab.

#include "ext4_vfs.h"
#include "device.h"
#include "serial.h"

namespace {
constexpr uint8_t EXT4_MAX_SLOTS = 4;
brook::Device* g_ext4Devices[EXT4_MAX_SLOTS] = {};
}

extern "C" bool Ext4BindDevice(uint8_t pdrv, brook::Device* dev)
{
    if (pdrv >= EXT4_MAX_SLOTS || !dev) return false;
    g_ext4Devices[pdrv] = dev;
    brook::SerialPrintf("ext4: bound device '%s' to pdrv %u\n", dev->name, pdrv);
    return true;
}

extern "C" brook::Device* Ext4GetDevice(uint8_t pdrv)
{
    if (pdrv >= EXT4_MAX_SLOTS) return nullptr;
    return g_ext4Devices[pdrv];
}
