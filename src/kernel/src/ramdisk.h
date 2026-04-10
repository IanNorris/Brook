#pragma once

#include "device.h"
#include <stdint.h>

// RamdiskDevice — a block device backed by a fixed memory region.

namespace brook {

// Private state for a ramdisk device.
// Stored in dev->priv; cast to RamdiskPriv* to access block size and data.
struct RamdiskPriv {
    uint8_t*  data;
    uint64_t  size;
    uint32_t  blockSize;
};

// Create and return a ramdisk device.
Device* RamdiskCreate(void* data, uint64_t size, uint32_t blockSize, const char* name);

} // namespace brook
