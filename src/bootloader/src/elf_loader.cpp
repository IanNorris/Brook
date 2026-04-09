#include "elf_loader.h"
#include "console.h"
#include "memory.h"
#include "elf.h"  // our minimal ELF types

namespace brook
{
namespace bootloader
{

// Physical address where the kernel is loaded.
// 4MB: well above legacy BIOS regions, typically free in UEFI environments.
static constexpr EFI_PHYSICAL_ADDRESS KernelPhysicalBase = 0x400000ULL;

KernelEntryFn LoadKernelElf(
    EFI_BOOT_SERVICES*    bootServices,
    const uint8_t*        elfData,
    UINTN                 elfSize,
    EFI_PHYSICAL_ADDRESS& outPhysBase,
    UINTN&                outPhysPages)
{
    if (elfSize < sizeof(Elf64Header))
    {
        Halt(EFI_INVALID_PARAMETER, u"ELF file too small");
    }

    const auto* header = reinterpret_cast<const Elf64Header*>(elfData);

    // Validate magic
    if (header->ident[0] != ElfMagic0 ||
        header->ident[1] != ElfMagic1 ||
        header->ident[2] != ElfMagic2 ||
        header->ident[3] != ElfMagic3)
    {
        Halt(EFI_INVALID_PARAMETER, u"Not a valid ELF file");
    }

    if (header->ident[4] != ElfClass64)
    {
        Halt(EFI_UNSUPPORTED, u"Kernel must be a 64-bit ELF");
    }

    if (header->machine != ElfMachineX86_64)
    {
        Halt(EFI_UNSUPPORTED, u"Kernel must target x86-64");
    }

    if (header->type != ElfTypeExec && header->type != ElfTypeDyn)
    {
        Halt(EFI_UNSUPPORTED, u"Kernel ELF must be executable or dynamic");
    }

    if (header->phCount == 0 || header->phEntSize < sizeof(Elf64ProgramHeader))
    {
        Halt(EFI_INVALID_PARAMETER, u"ELF has no valid program headers");
    }

    // Find the span of all PT_LOAD segments (in virtual address space)
    static constexpr uint64_t kMaxU64 = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t lowestVAddr  = kMaxU64;
    uint64_t highestVAddr = 0;

    for (uint16_t i = 0; i < header->phCount; i++)
    {
        const auto* ph = reinterpret_cast<const Elf64ProgramHeader*>(
            elfData + header->phOffset + i * header->phEntSize);

        if (ph->type != PtLoad || ph->memSize == 0)
        {
            continue;
        }

        if (ph->vaddr < lowestVAddr)
        {
            lowestVAddr = ph->vaddr;
        }

        if (ph->vaddr + ph->memSize > highestVAddr)
        {
            highestVAddr = ph->vaddr + ph->memSize;
        }
    }

    if (lowestVAddr == kMaxU64 || highestVAddr == 0)
    {
        Halt(EFI_INVALID_PARAMETER, u"ELF has no loadable segments");
    }

    UINT64 kernelSize = highestVAddr - lowestVAddr;
    UINT64 kernelPages = (kernelSize + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;

    ConsolePrint(u"Kernel size: ");
    ConsolePrintDec(kernelSize);
    ConsolePrintLine(u" bytes");

    // Allocate physical pages for the kernel.
    // The kernel is linked with lowestVAddr == KernelPhysicalBase so virtual == physical for now.
    EFI_PHYSICAL_ADDRESS physBase = KernelPhysicalBase;
    EFI_STATUS status = bootServices->AllocatePages(
        AllocateAddress,
        EfiLoaderData,
        kernelPages,
        &physBase);

    if (EFI_ERROR(status))
    {
        ConsolePrintLine(u"Warning: preferred kernel address unavailable, using any");
        physBase = 0;
        status = bootServices->AllocatePages(
            AllocateAnyPages,
            EfiLoaderData,
            kernelPages,
            &physBase);

        if (EFI_ERROR(status))
        {
            Halt(status, u"Failed to allocate kernel pages");
        }
    }

    ConsolePrint(u"Kernel physical base: ");
    ConsolePrintHex(physBase);
    ConsolePrintLine(u"");

    // Load each PT_LOAD segment
    for (uint16_t i = 0; i < header->phCount; i++)
    {
        const auto* ph = reinterpret_cast<const Elf64ProgramHeader*>(
            elfData + header->phOffset + i * header->phEntSize);

        if (ph->type != PtLoad || ph->memSize == 0)
        {
            continue;
        }

        // Destination = physBase + (vaddr - lowestVAddr)
        uint8_t* dest = reinterpret_cast<uint8_t*>(physBase + (ph->vaddr - lowestVAddr));

        // Copy initialised data from the ELF file
        if (ph->fileSize > 0)
        {
            bootServices->CopyMem(dest, const_cast<uint8_t*>(elfData + ph->offset), ph->fileSize);
        }

        // Zero uninitialised data (BSS)
        if (ph->memSize > ph->fileSize)
        {
            bootServices->SetMem(dest + ph->fileSize, ph->memSize - ph->fileSize, 0);
        }
    }

    outPhysBase  = physBase;
    outPhysPages = kernelPages;

    // Return the VIRTUAL entry point from the ELF header.
    // This is the address the CPU will jump to AFTER our page tables are loaded.
    ConsolePrint(u"Kernel virtual entry: ");
    ConsolePrintHex(header->entry);
    ConsolePrintLine(u"");

    return reinterpret_cast<KernelEntryFn>(header->entry);
}

} // namespace bootloader
} // namespace brook
