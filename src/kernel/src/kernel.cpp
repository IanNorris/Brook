#include "boot_protocol/boot_protocol.h"
#include "serial.h"
#include "gdt.h"
#include "idt.h"
#include "pmm.h"

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
    brook::SerialInit();
    brook::SerialPrintf("Brook kernel starting...\n");

    // Validate boot protocol
    if (bootProtocol == nullptr ||
        bootProtocol->magic != brook::BootProtocolMagic)
    {
        for (;;) { __asm__ volatile("hlt"); }
    }

    GdtInit();
    brook::SerialPuts("GDT loaded\n");

    IdtInit(&bootProtocol->framebuffer);
    brook::SerialPuts("IDT loaded\n");

    brook::PmmInit(bootProtocol);
    brook::SerialPrintf("PMM ready: %u free pages (%u MB)\n",
                        static_cast<uint32_t>(brook::PmmGetFreePageCount()),
                        static_cast<uint32_t>((brook::PmmGetFreePageCount() * 4096) / (1024*1024)));

    const brook::Framebuffer& fb = bootProtocol->framebuffer;
    brook::SerialPrintf("Framebuffer: %ux%u @ 0x%p\n",
                        fb.width, fb.height,
                        reinterpret_cast<void*>(fb.physicalBase));

    // Clear to dark blue to show the kernel owns the screen now
    DrawRect(fb, 0, 0, fb.width, fb.height, 0x00001A3A);

    // Draw a white rectangle as proof-of-life
    DrawRect(fb, 50, 50, 200, 100, 0x00FFFFFF);

    brook::SerialPuts("Kernel running — waiting for interrupts\n");

    // Enable interrupts and halt
    __asm__ volatile("sti");
    for (;;) { __asm__ volatile("hlt"); }
}
