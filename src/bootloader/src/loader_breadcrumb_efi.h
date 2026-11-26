#pragma once
#include <Uefi.h>
#include <stdint.h>
namespace brook {
namespace bootloader {
// Loader breadcrumbs (loader_breadcrumb.h): LOADER.TXT / HALT.TXT on the loader's volume.
void BreadcrumbEntry(EFI_HANDLE image, EFI_BOOT_SERVICES* bs);
void BreadcrumbGop(bool gop, uint32_t w, uint32_t h, uint32_t modes, bool setmode_tried, bool setmode_failed, uint64_t status);
void BreadcrumbKernel();   // last write; the loader is about to exit boot services
void BreadcrumbHalt(uint64_t status, const char16_t* message);
}
}
