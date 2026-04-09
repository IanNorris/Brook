#include <Uefi.h>
#include "console.h"
#include "config.h"
#include "graphics.h"
#include "memory.h"
#include "acpi.h"
#include "elf_loader.h"
#include "fs.h"
#include "paging.h"
#include "boot_protocol/boot_protocol.h"

extern "C" EFI_STATUS EFIAPI EfiMain(EFI_HANDLE imageHandle, EFI_SYSTEM_TABLE* systemTable)
{
    using namespace brook::bootloader;

    ConsoleInit(systemTable->ConOut);
    ConsolePrintLine(u"Brook bootloader starting...");

    // --- Config ---
    LoadConfig(imageHandle, systemTable->BootServices);

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
    UINTN kernelFileSize = 0;
    uint8_t* kernelData = ReadFile(imageHandle, systemTable->BootServices,
        g_bootConfig.target, &kernelFileSize);
    if (kernelData == nullptr)
    {
        Halt(EFI_NOT_FOUND, u"Kernel not found");
    }
    ConsolePrintLine(u"Kernel file read");

    EFI_PHYSICAL_ADDRESS kernelPhysBase = 0;
    UINTN kernelPhysPages = 0;
    KernelEntryFn kernelVirtualEntry = LoadKernelElf(
        systemTable->BootServices, kernelData, kernelFileSize, kernelPhysBase, kernelPhysPages);
    ConsolePrintLine(u"Kernel loaded");

    // Free the temporary ELF file buffer (segments already copied out)
    FreePages(systemTable->BootServices,
        reinterpret_cast<EFI_PHYSICAL_ADDRESS>(kernelData), kernelFileSize);

    // --- Allocate page table memory (must be before ExitBootServices) ---
    // Pass kernelPhysPages so AllocatePageTables can size the kernel PT pool.
    // It also scans the UEFI memory map internally to size the identity map.
    PageTableAllocation pageTableAlloc = AllocatePageTables(systemTable->BootServices, kernelPhysPages);
    ConsolePrintLine(u"Page table memory allocated");

    // --- Memory map (must be last thing before ExitBootServices) ---
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

    // From here: no UEFI services available. No console output.

    // --- Build and activate page tables ---
    // After this, virtual 0xFFFFFFFF80000000 maps to kernelPhysBase.
    // Low memory 0-4GB is identity-mapped so bootloader code continues running.
    BuildPageTables(pageTableAlloc, kernelPhysBase, kernelPhysPages);
    LoadCR3(pageTableAlloc);

    // --- Build boot protocol ---
    // This struct is on the stack (in identity-mapped low memory), accessible
    // from both the current low address and after the kernel takes over.
    brook::BootProtocol bootProtocol{};
    bootProtocol.magic          = brook::BootProtocolMagic;
    bootProtocol.version        = brook::BootProtocolVersion;
    bootProtocol.framebuffer    = framebuffer;
    bootProtocol.acpi           = acpi;
    bootProtocol.memoryMap      = memoryMap;
    bootProtocol.memoryMapCount = memoryMapCount;
    bootProtocol.kernelPhysBase  = kernelPhysBase;
    bootProtocol.kernelPhysPages = kernelPhysPages;

    // --- Jump to kernel at virtual address ---
    // Use inline asm to ensure SysV calling convention:
    // first argument (bootProtocol ptr) goes in RDI, not RCX.
    {
        void* entry    = reinterpret_cast<void*>(kernelVirtualEntry);
        void* protocol = &bootProtocol;
        asm volatile(
            "mov %1, %%rdi\n\t"
            "jmp *%0\n\t"
            :
            : "r"(entry), "r"(protocol)
            : "rdi"
        );
    }

    // Should never reach here
    for (;;) { __asm__ volatile("hlt"); }
}
