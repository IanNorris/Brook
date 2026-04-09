#pragma once
#include <stdint.h>

namespace brook {

void SerialInit();
void SerialPutChar(char c);
void SerialPuts(const char* str);
// Minimal printf-like: supports %s %d %u %x %lu %lx %ld %p %c %%
void SerialPrintf(const char* fmt, ...);
void SerialVPrintf(const char* fmt, __builtin_va_list args);

} // namespace brook
