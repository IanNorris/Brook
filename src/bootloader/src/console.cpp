#include "console.h"

namespace brook
{
namespace bootloader
{

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* gConOut = nullptr;

void ConsoleInit(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* conOut)
{
    gConOut = conOut;
    gConOut->ClearScreen(gConOut);
}

void ConsolePrint(const char16_t* message)
{
    if (gConOut != nullptr)
    {
        gConOut->OutputString(gConOut, (CHAR16*)message);
    }
}

void ConsolePrintLine(const char16_t* message)
{
    ConsolePrint(message);
    ConsolePrint(u"\r\n");
}

void ConsolePrintHex(UINT64 value)
{
    char16_t buf[19]; // "0x" + 16 hex digits + null
    buf[0]  = u'0';
    buf[1]  = u'x';
    buf[18] = u'\0';

    for (int i = 0; i < 16; i++)
    {
        UINT64 nibble = (value >> (60 - i * 4)) & 0xFu;
        buf[2 + i]    = (char16_t)(nibble < 10 ? u'0' + nibble : u'A' + nibble - 10);
    }

    ConsolePrint(buf);
}

void ConsolePrintDec(UINT64 value)
{
    char16_t buf[21]; // max uint64 is 20 digits + null
    buf[20] = u'\0';

    if (value == 0)
    {
        ConsolePrint(u"0");
        return;
    }

    int pos = 20;
    while (value > 0)
    {
        buf[--pos] = (char16_t)(u'0' + (value % 10));
        value /= 10;
    }

    ConsolePrint(buf + pos);
}

[[noreturn]] void Halt(EFI_STATUS status, const char16_t* message)
{
    ConsolePrint(u"\r\nFATAL ERROR (");
    ConsolePrintHex((UINT64)status);
    ConsolePrint(u"): ");
    ConsolePrintLine(message);

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}

} // namespace bootloader
} // namespace brook
