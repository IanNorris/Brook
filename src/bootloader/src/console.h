#pragma once
#include <Uefi.h>

namespace brook
{
namespace bootloader
{

void ConsoleInit(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* conOut);
void ConsolePrint(const char16_t* message);
void ConsolePrintLine(const char16_t* message);
void ConsolePrintHex(UINT64 value);
void ConsolePrintDec(UINT64 value);
[[noreturn]] void Halt(EFI_STATUS status, const char16_t* message);

} // namespace bootloader
} // namespace brook
