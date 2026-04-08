#include "fs.h"
#include "memory.h"
#include "console.h"

#include "Protocol/LoadedImage.h"
#include "Protocol/SimpleFileSystem.h"
#include "Guid/FileInfo.h"

namespace brook
{
namespace bootloader
{

uint8_t* ReadFile(
    EFI_HANDLE         imageHandle,
    EFI_BOOT_SERVICES* bootServices,
    const char16_t*    path,
    UINTN*             outSize)
{
    EFI_GUID loadedImageGuid        = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID simpleFileSystemGuid   = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID fileInfoGuid           = EFI_FILE_INFO_ID;

    EFI_LOADED_IMAGE_PROTOCOL* loadedImage = nullptr;
    EFI_STATUS status = bootServices->HandleProtocol(
        imageHandle, &loadedImageGuid, (void**)&loadedImage);
    if (EFI_ERROR(status)) { return nullptr; }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = nullptr;
    status = bootServices->OpenProtocol(
        loadedImage->DeviceHandle,
        &simpleFileSystemGuid,
        (void**)&fs,
        imageHandle,
        nullptr,
        EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(status)) { return nullptr; }

    EFI_FILE_PROTOCOL* root = nullptr;
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) { return nullptr; }

    EFI_FILE_PROTOCOL* file = nullptr;
    status = root->Open(root, &file, (CHAR16*)path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status))
    {
        root->Close(root);
        return nullptr;  // file not found - caller handles this
    }

    // Get file size via EFI_FILE_INFO
    UINT8 infoBuffer[256];
    UINTN infoSize = sizeof(infoBuffer);
    status = file->GetInfo(file, &fileInfoGuid, &infoSize, infoBuffer);
    if (EFI_ERROR(status))
    {
        file->Close(file);
        root->Close(root);
        return nullptr;
    }

    EFI_FILE_INFO* info = (EFI_FILE_INFO*)infoBuffer;
    UINTN fileSize = (UINTN)info->FileSize;

    EFI_PHYSICAL_ADDRESS bufferAddr = AllocatePages(bootServices, EfiLoaderData, fileSize);
    uint8_t* buffer = (uint8_t*)bufferAddr;

    UINTN readSize = fileSize;
    status = file->Read(file, &readSize, buffer);
    file->Close(file);
    root->Close(root);

    if (EFI_ERROR(status))
    {
        FreePages(bootServices, bufferAddr, fileSize);
        return nullptr;
    }

    *outSize = fileSize;
    return buffer;
}

} // namespace bootloader
} // namespace brook
