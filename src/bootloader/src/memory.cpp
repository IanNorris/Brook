#include "memory.h"
#include "console.h"

namespace brook
{
namespace bootloader
{

static UINT64 PageCount(UINT64 sizeInBytes)
{
    return (sizeInBytes + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
}

EFI_PHYSICAL_ADDRESS AllocatePages(EFI_BOOT_SERVICES* bootServices, EFI_MEMORY_TYPE pageType, UINT64 sizeInBytes)
{
    EFI_PHYSICAL_ADDRESS address = 0;
    UINT64 pages                 = PageCount(sizeInBytes);

    EFI_STATUS status = bootServices->AllocatePages(AllocateAnyPages, pageType, pages, &address);
    if (EFI_ERROR(status))
    {
        Halt(status, u"AllocatePages failed");
    }

    return address;
}

void FreePages(EFI_BOOT_SERVICES* bootServices, EFI_PHYSICAL_ADDRESS address, UINT64 sizeInBytes)
{
    bootServices->FreePages(address, PageCount(sizeInBytes));
}

static brook::MemoryType ConvertMemoryType(UINT32 uefiType)
{
    switch ((EFI_MEMORY_TYPE)uefiType)
    {
        case EfiConventionalMemory:
            return brook::MemoryType::Free;
        case EfiReservedMemoryType:
        case EfiUnusableMemory:
            return brook::MemoryType::Reserved;
        case EfiACPIReclaimMemory:
            return brook::MemoryType::AcpiReclaimable;
        case EfiACPIMemoryNVS:
            return brook::MemoryType::AcpiNvs;
        case EfiMemoryMappedIO:
        case EfiMemoryMappedIOPortSpace:
            return brook::MemoryType::Mmio;
        case EfiLoaderCode:
            return brook::MemoryType::BootloaderCode;
        case EfiLoaderData:
            return brook::MemoryType::BootData;
        default:
            return brook::MemoryType::Reserved;
    }
}

UINTN BuildMemoryMap(
    EFI_BOOT_SERVICES*        bootServices,
    brook::MemoryDescriptor** outMap,
    UINT32*                   outCount)
{
    UINTN  mapSize         = 0;
    UINTN  mapKey          = 0;
    UINTN  descriptorSize  = 0;
    UINT32 descriptorVer   = 0;

    // Step 1: query required size
    EFI_STATUS status = bootServices->GetMemoryMap(&mapSize, nullptr, &mapKey, &descriptorSize, &descriptorVer);
    if (status != EFI_BUFFER_TOO_SMALL && EFI_ERROR(status))
    {
        Halt(status, u"GetMemoryMap (size query) failed");
    }

    // Add headroom for descriptors added by our allocation below
    UINTN allocSize = mapSize + (8 * descriptorSize);

    EFI_PHYSICAL_ADDRESS bufferAddr = AllocatePages(bootServices, EfiLoaderData, allocSize);
    EFI_MEMORY_DESCRIPTOR* buffer   = (EFI_MEMORY_DESCRIPTOR*)bufferAddr;

    int retries = 5;
    do
    {
        mapSize = allocSize;
        status  = bootServices->GetMemoryMap(&mapSize, buffer, &mapKey, &descriptorSize, &descriptorVer);

        if (status == EFI_BUFFER_TOO_SMALL)
        {
            FreePages(bootServices, bufferAddr, allocSize);

            // Re-query size and reallocate with headroom
            mapSize = 0;
            EFI_STATUS sizeStatus = bootServices->GetMemoryMap(&mapSize, nullptr, nullptr, &descriptorSize, nullptr);
            if (sizeStatus != EFI_BUFFER_TOO_SMALL && EFI_ERROR(sizeStatus))
            {
                Halt(sizeStatus, u"GetMemoryMap (re-query) failed");
            }

            allocSize   = mapSize + (8 * descriptorSize);
            bufferAddr  = AllocatePages(bootServices, EfiLoaderData, allocSize);
            buffer      = (EFI_MEMORY_DESCRIPTOR*)bufferAddr;
        }
    } while (status == EFI_BUFFER_TOO_SMALL && retries-- > 0);

    if (EFI_ERROR(status))
    {
        Halt(status, u"GetMemoryMap failed");
    }

    UINTN entryCount = mapSize / descriptorSize;

    // Allocate our output descriptor array
    UINT64 outArraySize                  = entryCount * sizeof(brook::MemoryDescriptor);
    EFI_PHYSICAL_ADDRESS outArrayAddr    = AllocatePages(bootServices, EfiLoaderData, outArraySize);
    brook::MemoryDescriptor* descriptors = (brook::MemoryDescriptor*)outArrayAddr;

    for (UINTN i = 0; i < entryCount; i++)
    {
        EFI_MEMORY_DESCRIPTOR* src = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)buffer + i * descriptorSize);
        brook::MemoryDescriptor& dst = descriptors[i];

        dst.physicalStart = src->PhysicalStart;
        dst.pageCount     = src->NumberOfPages;
        dst.type          = ConvertMemoryType(src->Type);
        dst._reserved     = 0;
    }

    *outMap   = descriptors;
    *outCount = (UINT32)entryCount;

    return mapKey;
}

} // namespace bootloader
} // namespace brook
