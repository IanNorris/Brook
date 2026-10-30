#pragma once
#include <Uefi.h>
#include <stdint.h>
namespace brook { namespace bootloader {
// Returns a completely verified pair, or leaves both outputs null for legacy
// recovery. Buffers contain the declared bytes, never slot padding.
bool LoadUpdatePair(EFI_HANDLE image, EFI_BOOT_SERVICES* services,
    uint8_t*& kernel, UINTN& kernelBytes, uint8_t*& initrd, UINTN& initrdBytes);
}}
