#include <Uefi.h>
#include "console.h"
#include "graphics.h"
#include "memory.h"
#include "acpi.h"
#include "boot_protocol/boot_protocol.h"

extern "C" EFI_STATUS EFIAPI EfiMain(EFI_HANDLE imageHandle, EFI_SYSTEM_TABLE* systemTable)
{
    using namespace brook::bootloader;

    ConsoleInit(systemTable->ConOut);
    ConsolePrintLine(u"Brook bootloader starting...");

    // --- Graphics ---
    brook::Framebuffer framebuffer{};
    if (!GraphicsInit(systemTable->BootServices, framebuffer))
    {
        Halt(EFI_UNSUPPORTED, u"No GOP framebuffer available");
    }
    ConsolePrintLine(u"Framebuffer initialized");

    // --- ACPI ---
    brook::AcpiInfo acpi{};
    if (!AcpiInit(systemTable, acpi))
    {
        Halt(EFI_NOT_FOUND, u"ACPI 2.0 not found");
    }
    ConsolePrintLine(u"ACPI located");

    // --- Memory map (must be last before ExitBootServices) ---
    brook::MemoryDescriptor* memoryMap  = nullptr;
    UINT32 memoryMapCount               = 0;
    UINTN mapKey = BuildMemoryMap(systemTable->BootServices, &memoryMap, &memoryMapCount);
    ConsolePrintLine(u"Memory map built");

    // --- Exit boot services ---
    EFI_STATUS status = systemTable->BootServices->ExitBootServices(imageHandle, mapKey);
    if (EFI_ERROR(status))
    {
        // Map changed - retry once with fresh key
        mapKey = BuildMemoryMap(systemTable->BootServices, &memoryMap, &memoryMapCount);
        status = systemTable->BootServices->ExitBootServices(imageHandle, mapKey);
        if (EFI_ERROR(status))
        {
            Halt(status, u"ExitBootServices failed");
        }
    }

    // After ExitBootServices, we cannot use ConOut or BootServices.
    // Draw a pixel to the framebuffer to prove we got here.
    if (framebuffer.physicalBase != 0)
    {
        UINT32* fb = reinterpret_cast<UINT32*>(framebuffer.physicalBase);
        // Draw a 10x10 green square at (10,10)
        for (UINT32 y = 10; y < 20; y++)
        {
            for (UINT32 x = 10; x < 20; x++)
            {
                fb[y * (framebuffer.stride / 4) + x] = 0x0000FF00u; // green
            }
        }
    }

    // TODO: load kernel, set up page tables, jump

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}
