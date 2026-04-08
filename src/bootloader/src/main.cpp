#include <Uefi.h>
#include "console.h"
#include "graphics.h"
#include "memory.h"
#include "acpi.h"
#include "elf_loader.h"
#include "fs.h"
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

    // --- Load kernel ---
    UINTN kernelSize = 0;
    uint8_t* kernelData = ReadFile(imageHandle, systemTable->BootServices, u"KERNEL\\BROOK.ELF", &kernelSize);
    if (kernelData == nullptr)
    {
        Halt(EFI_NOT_FOUND, u"Kernel not found at KERNEL\\BROOK.ELF");
    }
    ConsolePrintLine(u"Kernel file read");

    KernelEntryFn kernelEntry = LoadKernelElf(systemTable->BootServices, kernelData, kernelSize);
    ConsolePrintLine(u"Kernel loaded");

    // Free the temporary ELF file buffer (segments already copied out)
    FreePages(systemTable->BootServices, (EFI_PHYSICAL_ADDRESS)(UINTN)kernelData, kernelSize);

    // --- Memory map (must be last before ExitBootServices) ---
    brook::MemoryDescriptor* memoryMap = nullptr;
    UINT32 memoryMapCount              = 0;
    UINTN mapKey = BuildMemoryMap(systemTable->BootServices, &memoryMap, &memoryMapCount);
    ConsolePrintLine(u"Memory map built");

    // --- Exit boot services ---
    EFI_STATUS status = systemTable->BootServices->ExitBootServices(imageHandle, mapKey);
    if (EFI_ERROR(status))
    {
        mapKey = BuildMemoryMap(systemTable->BootServices, &memoryMap, &memoryMapCount);
        status = systemTable->BootServices->ExitBootServices(imageHandle, mapKey);
        if (EFI_ERROR(status))
        {
            Halt(status, u"ExitBootServices failed");
        }
    }

    // Build boot protocol to hand to the kernel
    brook::BootProtocol bootProtocol{};
    bootProtocol.magic          = brook::BootProtocolMagic;
    bootProtocol.version        = brook::BootProtocolVersion;
    bootProtocol.framebuffer    = framebuffer;
    bootProtocol.acpi           = acpi;
    bootProtocol.memoryMap      = memoryMap;
    bootProtocol.memoryMapCount = memoryMapCount;

    // Jump to kernel
    kernelEntry(&bootProtocol);

    // Should never reach here
    for (;;) { __asm__ volatile("hlt"); }
}
