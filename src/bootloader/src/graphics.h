#pragma once
#include <Uefi.h>
#include "boot_protocol/boot_protocol.h"

namespace brook
{
namespace bootloader
{

// No GOP: a reserved RAM surface so the kernel can boot headless.
bool HeadlessFramebuffer(EFI_BOOT_SERVICES* bootServices, brook::Framebuffer& outFramebuffer);

// Query GOP and fill in the Framebuffer struct.
// Tries to find the best available mode (prefer 1920x1080, fallback to largest available).
// Returns false if GOP is unavailable.
bool GraphicsInit(EFI_BOOT_SERVICES* bootServices, brook::Framebuffer& outFramebuffer);

} // namespace bootloader
} // namespace brook
