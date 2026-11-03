#pragma once
#include <Uefi.h>
#include "Protocol/BlockIo.h"
#include "recovery_device_path.h"
#include "recovery_disk_read.h"

namespace brook { namespace bootloader {
struct RecoveryFirmwareDisk {
    EFI_HANDLE handle = nullptr;
    EFI_BLOCK_IO_PROTOCOL* whole = nullptr;
    EFI_BLOCK_IO_PROTOCOL* esp = nullptr;
    EFI_BLOCK_IO_MEDIA* wholeMedia = nullptr;
    EFI_BLOCK_IO_MEDIA* espMedia = nullptr;
    UINT32 wholeMediaId = 0, espMediaId = 0, wholeAlign = 0, espAlign = 0;
    EFI_LBA wholeLast = 0, espLast = 0;
    RecoveryEspPath espPath{};
};
// Locate the unique whole-disk Block I/O handle whose device path is the
// exact parent of the firmware-loaded GPT ESP. This function only enumerates
// protocols; it neither reads nor writes disk contents.
bool RecoveryFindFirmwareDisk(EFI_HANDLE image,EFI_BOOT_SERVICES* bs,RecoveryFirmwareDisk& out);
// Inspect only; expected disk/data/filesystem identity and data bounds must
// come from independently provisioned trusted configuration. No writes.
RecoveryDiskState RecoveryInspectFirmwareDisk(EFI_HANDLE image,EFI_BOOT_SERVICES* bs,
                                               const RecoveryDiskIdentity& expected);
}}
