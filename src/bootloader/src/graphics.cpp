#include "graphics.h"
#include "console.h"
#include "config.h"
#include "mode_selection.h"
#include "loader_breadcrumb_efi.h"
#include "Protocol/GraphicsOutput.h"

namespace brook
{
namespace bootloader
{

// No GOP: a 1920x1080 BGRX surface in EfiReservedMemoryType pages (Reserved to the
// kernel, never reclaimed), zeroed. Nothing scans it out.
bool HeadlessFramebuffer(EFI_BOOT_SERVICES* bootServices, brook::Framebuffer& outFramebuffer)
{
    constexpr UINT32 w = 1920, h = 1080;
    const UINTN pages = (UINTN(w) * h * 4 + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS address = 0;
    if (EFI_ERROR(bootServices->AllocatePages(AllocateAnyPages, EfiReservedMemoryType, pages, &address)) || !address)
        return false;
    bootServices->SetMem(reinterpret_cast<VOID*>(address), pages * 4096, 0);
    outFramebuffer = brook::Framebuffer{};
    outFramebuffer.physicalBase = address;
    outFramebuffer.width = w; outFramebuffer.height = h; outFramebuffer.stride = w * 4;
    outFramebuffer.format = brook::PixelFormat::Bgr8;
    return true;
}

bool GraphicsInit(EFI_BOOT_SERVICES* bootServices, brook::Framebuffer& outFramebuffer)
{
    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = nullptr;

    EFI_STATUS status = bootServices->LocateProtocol(&gopGuid, nullptr, (VOID**)&gop);
    if (EFI_ERROR(status) || gop == nullptr)
    {
        BreadcrumbGop(false, 0, 0, 0, false, false, status);
        return false;
    }

    UINT32 bestMode    = gop->Mode->Mode; // current mode as fallback
    UINT32 bestWidth   = 0;
    UINT32 bestHeight  = 0;

    UINT32 maxMode = gop->Mode->MaxMode;
    for (UINT32 mode = 0; mode < maxMode; mode++)
    {
        UINTN                              infoSize = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info = nullptr;

        status = gop->QueryMode(gop, mode, &infoSize, &info);
        if (EFI_ERROR(status) || info == nullptr)
        {
            continue;
        }

        UINT32 w = info->HorizontalResolution;
        UINT32 h = info->VerticalResolution;

        if (info->PixelFormat != PixelBltOnly &&
            PreferMode(w,h,bestWidth,bestHeight,
                       g_bootConfig.displayWidth,g_bootConfig.displayHeight))
        {
            bestMode=mode; bestWidth=w; bestHeight=h;
        }
        bootServices->FreePool(info);

    }

    // A failed SetMode keeps the firmware's current mode (it is valid: GOP reported it);
    // halting here left the machine dark with no way to tell why.
    const EFI_STATUS setStatus = gop->SetMode(gop, bestMode);
    if (EFI_ERROR(setStatus) && !gop->Mode->Info)
    {
        BreadcrumbGop(false, 0, 0, maxMode, true, true, setStatus);
        return false;
    }

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* modeInfo = gop->Mode->Info;

    // Map GOP pixel format to our enum
    brook::PixelFormat pixelFormat;
    brook::PixelBitmask bitmask{};

    switch (modeInfo->PixelFormat)
    {
        case PixelBlueGreenRedReserved8BitPerColor:
            pixelFormat = brook::PixelFormat::Bgr8;
            break;
        case PixelRedGreenBlueReserved8BitPerColor:
            pixelFormat = brook::PixelFormat::Rgb8;
            break;
        case PixelBitMask:
            pixelFormat      = brook::PixelFormat::Bitmask;
            bitmask.red      = modeInfo->PixelInformation.RedMask;
            bitmask.green    = modeInfo->PixelInformation.GreenMask;
            bitmask.blue     = modeInfo->PixelInformation.BlueMask;
            bitmask.reserved = modeInfo->PixelInformation.ReservedMask;
            break;
        default:
            // Unsupported format
            return false;
    }

    outFramebuffer.physicalBase = (UINT64)gop->Mode->FrameBufferBase;
    outFramebuffer.width        = modeInfo->HorizontalResolution;
    outFramebuffer.height       = modeInfo->VerticalResolution;
    outFramebuffer.stride       = modeInfo->PixelsPerScanLine * 4;
    BreadcrumbGop(true, modeInfo->HorizontalResolution, modeInfo->VerticalResolution, maxMode, true,
                  EFI_ERROR(setStatus), setStatus);
    outFramebuffer.format       = pixelFormat;
    outFramebuffer.bitmask      = bitmask;

    return true;
}

} // namespace bootloader
} // namespace brook
