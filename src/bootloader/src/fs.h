#pragma once
#include <Uefi.h>
#include <stdint.h>

namespace brook
{
namespace bootloader
{

// Read the entire contents of a file from the EFI System Partition into a
// freshly allocated EfiLoaderData buffer. Sets *outSize to the file size.
// Returns a pointer to the buffer on success, nullptr on failure.
// maximumSize bounds the on-disk file before allocating; prefixSize reads only
// that many bytes of a preallocated slot (and must not exceed its file size).
// The caller is responsible for freeing the buffer via FreePages.
uint8_t* ReadFile(
    EFI_HANDLE          imageHandle,
    EFI_BOOT_SERVICES*  bootServices,
    const char16_t*     path,
    UINTN*              outSize,
    UINTN               maximumSize = 512u * 1024u * 1024u,
    UINTN               prefixSize = 0);

} // namespace bootloader
} // namespace brook
