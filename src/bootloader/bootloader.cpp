
#include <Uefi.h>

extern "C" EFI_STATUS EFIAPI EfiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable)
{
    (void)ImageHandle;

    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)u"Hello UEFI");
    while(1);
    return EFI_SUCCESS;
}
