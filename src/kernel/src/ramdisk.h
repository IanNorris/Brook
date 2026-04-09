#pragma once

#include "device.h"
#include <stdint.h>

// RamdiskDevice — a block device backed by a fixed memory region.
//
// Useful for testing FatFS and the VFS layer without real storage hardware.
// The memory buffer must outlive the device (typically a static or heap array).
//
// Usage:
//   static uint8_t buf[512 * 2048];  // 1 MiB ramdisk
//   Device* rd = RamdiskCreate(buf, sizeof(buf), 512, "ramdisk0");
//   DeviceRegister(rd);

namespace brook {

// Create and return a ramdisk device.
// data     : backing memory (must remain valid for the device's lifetime)
// size     : total size in bytes (must be a multiple of block_size)
// blockSize: logical block size (typically 512 or 4096)
// name     : device name registered in the device table
//
// Returns a pointer to a statically allocated Device on success,
// or nullptr if parameters are invalid or the heap is exhausted.
Device* RamdiskCreate(void* data, uint64_t size, uint32_t blockSize, const char* name);

} // namespace brook
