#pragma once

#include "device.h"
#include <stdint.h>

// virtio-blk block device driver (legacy PCI interface).
//
// Scans PCI for vendor=0x1AF4, device=0x1001 (virtio-blk legacy).
// Sets up virtqueue 0, registers as a DEV_BLOCK device named "virtio0".
//
// QEMU flags to expose a disk image as virtio-blk:
//   -drive if=virtio,format=raw,file=<path/to/disk.img>
//
// Usage:
//   VirtioBlkInit();  // call after VfsInit(); registers device if found

namespace brook {

// Scan PCI, initialise the first virtio-blk device found, and register it
// in the device registry.  Returns the Device* on success, nullptr if no
// virtio-blk device is present or initialisation fails.
Device* VirtioBlkInit();

} // namespace brook
