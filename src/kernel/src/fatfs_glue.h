#pragma once

#include <stdint.h>
#include "device.h"

// FatFS glue — binds our Device registry to FatFS's diskio interface.
//
// Usage:
//   FatFsBindDrive(0, ramdiskDevice);   // map physical drive 0 → device
//   FATFS fs; f_mount(&fs, "0:", 1);    // mount volume "0:" on drive 0

extern "C" {
#include "ff.h"
}

namespace brook {

// Bind a block Device to a FatFS physical drive number (0-3).
// Must be called before f_mount() for that drive.
bool FatFsBindDrive(uint8_t pdrv, Device* dev);

} // namespace brook
