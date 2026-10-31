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
int FatFsFlushDrive(uint8_t pdrv);
// One actual FatFS instance per drive. Keeps the binding and its immutable
// read-only capability stable until the last retained mount is destroyed.
bool FatFsAcquireDrive(uint8_t pdrv, bool* readOnly);
void FatFsReleaseDrive(uint8_t pdrv);
bool FatFsDriveReadOnly(uint8_t pdrv);
// Borrowed boot-lifetime binding; caller retains the FAT mount/lease.
Device* FatFsBoundDevice(uint8_t pdrv);

} // namespace brook
