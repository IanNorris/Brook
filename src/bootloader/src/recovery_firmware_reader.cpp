#include <Uefi.h>
#include "Protocol/BlockIo.h"
#include "recovery_firmware_reader.h"
#include "recovery_device_path.h"
#include "recovery_disk_read.h"

// Compile the read-only adapter against the actual firmware ABI even before
// guarded boot begins using it. This does not perform any firmware I/O.
template class brook::bootloader::RecoveryFirmwareReader<EFI_BLOCK_IO_PROTOCOL>;
template brook::bootloader::RecoveryDiskState brook::bootloader::RecoveryReadDiskState(
    brook::bootloader::RecoveryFirmwareReader<EFI_BLOCK_IO_PROTOCOL>&, uint64_t,
    const brook::bootloader::RecoveryDiskIdentity&, uint8_t*, uint8_t*, uint32_t);
