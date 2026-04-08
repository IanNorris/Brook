#include <Uefi.h>

// EFI entry point - called by UEFI firmware.
// ImageHandle: handle for this loaded image
// SystemTable: pointer to the UEFI system table (gateway to all UEFI services)
extern "C" EFI_STATUS EFIAPI EfiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable)
{
    (void)ImageHandle;

    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* cout = SystemTable->ConOut;

    cout->ClearScreen(cout);
    cout->OutputString(cout, (CHAR16*)u"Brook bootloader starting...\r\n");

    // Halt - will be replaced with kernel loading
    for (;;)
    {
        __asm__ volatile("hlt");
    }

    return EFI_SUCCESS;
}
