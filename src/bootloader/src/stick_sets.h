#pragma once
#include <Uefi.h>
#include <stdint.h>
#include "boot_protocol/boot_protocol.h"
namespace brook { namespace bootloader {
// STICK_SETS=1: verify SET.BIN (root) or PREV/SET.BIN and load that pair.
// Consumes TRIAL.BIN (renamed to TRIAL.RUN) before a trial boot; an
// unconfirmed TRIAL.RUN selects PREV first. Fills the boot volume identity.
// Returns false (no buffers) when neither set verifies.
bool LoadStickSet(EFI_HANDLE image, EFI_BOOT_SERVICES* bs, uint8_t*& kernel, UINTN& kernelBytes,
                  uint8_t*& initrd, UINTN& initrdBytes, BootStick& out);
}}
