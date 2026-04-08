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

    // Query required buffer size (expect EFI_BUFFER_TOO_SMALL)
    EFI_STATUS status = bootServices->GetMemoryMap(&mapSize, nullptr, &mapKey, &descriptorSize, &descriptorVer);
    if (status != EFI_BUFFER_TOO_SMALL && EFI_ERROR(status))
    {
        Halt(status, u"GetMemoryMap (size query) failed");
    }

    // Allocate both buffers BEFORE the final GetMemoryMap call so the key
    // we return reflects a map state AFTER all our allocations are done.
    // Extra headroom absorbs any descriptors added by our own AllocatePages calls.
    const UINTN Headroom   = 16;
    UINTN maxEntries       = (mapSize / descriptorSize) + Headroom;
    UINTN uefiAllocSize    = maxEntries * descriptorSize;
    UINTN outAllocSize     = maxEntries * sizeof(brook::MemoryDescriptor);

    EFI_PHYSICAL_ADDRESS uefiBufferAddr = AllocatePages(bootServices, EfiLoaderData, uefiAllocSize);
    EFI_PHYSICAL_ADDRESS outArrayAddr   = AllocatePages(bootServices, EfiLoaderData, outAllocSize);

    EFI_MEMORY_DESCRIPTOR*   uefiBuffer  = (EFI_MEMORY_DESCRIPTOR*)uefiBufferAddr;
    brook::MemoryDescriptor* descriptors = (brook::MemoryDescriptor*)outArrayAddr;

    // Final GetMemoryMap — no allocations after this until ExitBootServices.
    int retries = 5;
    do
    {
        mapSize = uefiAllocSize;
        status  = bootServices->GetMemoryMap(&mapSize, uefiBuffer, &mapKey, &descriptorSize, &descriptorVer);

        if (status == EFI_BUFFER_TOO_SMALL)
        {
            // Extremely unlikely: map grew beyond our headroom. Free and retry with more space.
            FreePages(bootServices, uefiBufferAddr, uefiAllocSize);
            FreePages(bootServices, outArrayAddr, outAllocSize);

            mapSize = 0;
            EFI_STATUS sizeStatus = bootServices->GetMemoryMap(&mapSize, nullptr, nullptr, &descriptorSize, nullptr);
            if (sizeStatus != EFI_BUFFER_TOO_SMALL && EFI_ERROR(sizeStatus))
            {
                Halt(sizeStatus, u"GetMemoryMap (re-query) failed");
            }

            maxEntries    = (mapSize / descriptorSize) + Headroom;
            uefiAllocSize = maxEntries * descriptorSize;
            outAllocSize  = maxEntries * sizeof(brook::MemoryDescriptor);

            uefiBufferAddr = AllocatePages(bootServices, EfiLoaderData, uefiAllocSize);
            outArrayAddr   = AllocatePages(bootServices, EfiLoaderData, outAllocSize);
            uefiBuffer     = (EFI_MEMORY_DESCRIPTOR*)uefiBufferAddr;
            descriptors    = (brook::MemoryDescriptor*)outArrayAddr;
        }
    } while (status == EFI_BUFFER_TOO_SMALL && retries-- > 0);

    if (EFI_ERROR(status))
    {
        Halt(status, u"GetMemoryMap failed");
    }

    // Convert UEFI descriptors to our format. No allocations here.
    UINTN entryCount = mapSize / descriptorSize;
    for (UINTN i = 0; i < entryCount; i++)
    {
        EFI_MEMORY_DESCRIPTOR* src   = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)uefiBuffer + i * descriptorSize);
        brook::MemoryDescriptor& dst = descriptors[i];

        dst.physicalStart = src->PhysicalStart;
        dst.pageCount     = src->NumberOfPages;
        dst.type          = ConvertMemoryType(src->Type);
        dst._reserved     = 0;
    }

    *outMap   = descriptors;
    *outCount = (UINT32)entryCount;

    // mapKey is valid: all allocations completed before the GetMemoryMap call that produced it.
    return mapKey;
}

} // namespace bootloader
} // namespace brook
