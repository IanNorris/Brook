#pragma once
#include <Uefi.h>
#include "boot_protocol/boot_protocol.h"

namespace brook
{
namespace bootloader
{

// Query GOP and fill in the Framebuffer struct.
// Tries to find the best available mode (prefer 1920x1080, fallback to largest available).
// Returns false if GOP is unavailable.
bool GraphicsInit(EFI_BOOT_SERVICES* bootServices, brook::Framebuffer& outFramebuffer);

} // namespace bootloader
} // namespace brook
