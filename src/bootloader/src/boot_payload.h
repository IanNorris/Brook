#pragma once

#include <Uefi.h>
#include "boot_compression.h"

namespace brook { namespace bootloader {

// On success a compressed input is replaced by a new page allocation. On
// error the caller retains the original file and its allocation length.
bool ExpandBootPayload(EFI_BOOT_SERVICES* bs, uint8_t*& file, UINTN& bytes,
                       BootPayloadKind kind, UINTN maximum);

}} // namespace brook::bootloader
