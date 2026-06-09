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

// BRO-179 stress-test toggle: when true, high-frequency hot-path SerialPrintf
// callers (TLB-shootdown forgiveness, per-execve PROFILE, compositor stats)
// skip their output. Serial is ~11KB/s and these per-operation logs throttle
// SMP throughput to serial speed; silencing them is required to drive the
// 64-CPU quarantine stress test at real speed. Set in PmmInit (co-located with
// the other stress toggles). Default false = normal verbose behaviour.
extern volatile bool g_hotLogQuiet;

} // namespace brook

// Debug-only logging — compiles to nothing in Release builds.
#ifdef NDEBUG
#define DbgPrintf(...) ((void)0)
#else
#define DbgPrintf(...) brook::SerialPrintf(__VA_ARGS__)
#endif
