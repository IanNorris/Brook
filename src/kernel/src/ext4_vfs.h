#pragma once

// ext4_vfs.h — Ext4 filesystem support (kernel-side device binding).
//
// The ext4 filesystem driver is implemented as a kernel module (ext4.mod)
// using the lwext4 library.  This header provides the kernel-side device
// binding table that the module reads during mount.

#include <stdint.h>

namespace brook {
struct Device;
}

// Bind a block device to an ext4 physical-drive slot (0-based, max 3).
// Must be called before VfsMount(..., "ext4", pdrv).
extern "C" bool Ext4BindDevice(uint8_t pdrv, brook::Device* dev);

// Retrieve the block device bound to a pdrv slot.
// Used by the ext4 module during mount.
extern "C" brook::Device* Ext4GetDevice(uint8_t pdrv);
