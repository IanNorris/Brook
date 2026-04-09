#pragma once
#include <stdint.h>

namespace brook {

void SerialInit();
void SerialPutChar(char c);
void SerialPuts(const char* str);
// Minimal printf-like: supports %s %d %u %x %p %c %%
void SerialPrintf(const char* fmt, ...);

} // namespace brook
