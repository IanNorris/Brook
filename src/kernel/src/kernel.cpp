#include "boot_protocol/boot_protocol.h"

// Draw a filled rectangle to the framebuffer using the given colour.
static void DrawRect(
    const brook::Framebuffer& fb,
    uint32_t x, uint32_t y,
    uint32_t w, uint32_t h,
    uint32_t colour)
{
    uint32_t* pixels = reinterpret_cast<uint32_t*>(fb.physicalBase);
    uint32_t stride  = fb.stride / 4;  // stride in pixels

    for (uint32_t row = y; row < y + h && row < fb.height; row++)
    {
        for (uint32_t col = x; col < x + w && col < fb.width; col++)
        {
            pixels[row * stride + col] = colour;
        }
    }
}

// Kernel entry point. Called by the bootloader via SysV ABI (argument in RDI).
// At this stage: UEFI page tables still active, running at physical address.
extern "C" __attribute__((sysv_abi)) void KernelMain(brook::BootProtocol* bootProtocol)
{
    // Validate boot protocol
    if (bootProtocol == nullptr ||
        bootProtocol->magic != brook::BootProtocolMagic)
    {
        for (;;) { __asm__ volatile("hlt"); }
    }

    const brook::Framebuffer& fb = bootProtocol->framebuffer;

    // Clear to dark blue to show the kernel owns the screen now
    DrawRect(fb, 0, 0, fb.width, fb.height, 0x00001A3A);

    // Draw a white rectangle as proof-of-life
    DrawRect(fb, 50, 50, 200, 100, 0x00FFFFFF);

    // Halt
    for (;;) { __asm__ volatile("hlt"); }
}
