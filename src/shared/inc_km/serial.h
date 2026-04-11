#pragma once
#include <stdint.h>

namespace brook {

// All functions have C linkage so driver modules can call them
// by unmangled name via the kernel symbol table (KsymLookup).

extern "C" void SerialInit();
extern "C" void SerialPutChar(char c);
extern "C" void SerialPuts(const char* str);
// Minimal printf-like: supports %s %d %u %x %lu %lx %ld %p %c %%
extern "C" void SerialPrintf(const char* fmt, ...);
extern "C" void SerialVPrintf(const char* fmt, __builtin_va_list args);

// Lock/unlock for callers that want to do atomic multi-call sequences
// (e.g. KPrintf serialises serial + TTY output together).
void SerialLock();
void SerialUnlock();

} // namespace brook
