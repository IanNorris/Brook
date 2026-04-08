#include "graphics.h"
#include "console.h"
#include "Protocol/GraphicsOutput.h"

namespace brook
{
namespace bootloader
{

bool GraphicsInit(EFI_BOOT_SERVICES* bootServices, brook::Framebuffer& outFramebuffer)
{
    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = nullptr;

    EFI_STATUS status = bootServices->LocateProtocol(&gopGuid, nullptr, (VOID**)&gop);
    if (EFI_ERROR(status) || gop == nullptr)
    {
        return false;
    }

    UINT32 bestMode    = gop->Mode->Mode; // current mode as fallback
    UINT32 bestWidth   = 0;
    UINT32 bestHeight  = 0;
    bool   foundIdeal  = false;

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

        // Skip blt-only modes (no linear framebuffer)
        if (info->PixelFormat == PixelBltOnly)
        {
            continue;
        }

        // Prefer 1920x1080, then 1280x720, then largest
        if (w == 1920 && h == 1080)
        {
            bestMode   = mode;
            bestWidth  = w;
            bestHeight = h;
            foundIdeal = true;
            break;
        }

        if (!foundIdeal)
        {
            if ((w == 1280 && h == 720) ||
                (w * h > bestWidth * bestHeight))
            {
                bestMode   = mode;
                bestWidth  = w;
                bestHeight = h;
            }
        }
    }

    status = gop->SetMode(gop, bestMode);
    if (EFI_ERROR(status))
    {
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
    outFramebuffer.format       = pixelFormat;
    outFramebuffer.bitmask      = bitmask;

    return true;
}

} // namespace bootloader
} // namespace brook
