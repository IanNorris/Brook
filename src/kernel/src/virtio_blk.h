#pragma once

#include "device.h"
#include <stdint.h>

// virtio-blk block device driver (legacy PCI interface).
//
// Scans PCI for vendor=0x1AF4, device=0x1001 (virtio-blk legacy).
// Sets up virtqueue 0, registers as DEV_BLOCK devices named "virtio0", "virtio1", ...
//
// QEMU flags to expose disk images as virtio-blk:
//   -drive if=virtio,format=raw,file=<path/to/disk.img>
//
// Usage:
//   uint32_t n = VirtioBlkInitAll();  // call after VfsInit(); registers all found devices

namespace brook {

// Scan PCI, initialise ALL virtio-blk devices found, register each in the device
// registry as "virtio0", "virtio1", ...  Returns the number of devices registered.
uint32_t VirtioBlkInitAll();

// Marker filename written to each disk image to identify its mount purpose.
// A disk containing this file at its root gets mounted at the path it specifies.
static constexpr const char* VIRTIO_ESP_MARKER = "BROOK.MNT";

} // namespace brook
