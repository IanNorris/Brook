#pragma once

// ext2_vfs.h — Ext2 filesystem driver for Brook VFS.

#include <stdint.h>

namespace brook {

struct Device;

// Register "ext2" filesystem driver with the VFS.
void Ext2VfsRegister();

} // namespace brook

// Bind a block device to an ext2 physical-drive slot (0-based).
// Must be called before VfsMount(..., "ext2", pdrv).
extern "C" bool Ext2BindDevice(uint8_t pdrv, brook::Device* dev);
