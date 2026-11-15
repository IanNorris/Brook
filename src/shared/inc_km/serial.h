#pragma once
#include <stdint.h>

namespace brook {

// All functions have C linkage so driver modules can call them
// by unmangled name via the kernel symbol table (KsymLookup).

extern "C" void SerialInit();
extern "C" void SerialPutChar(char c);
extern "C" void SerialPuts(const char* str);
// Nonblocking with respect to the serial lock and bounded on a stuck UART.
void SerialPanicBegin();
void SerialPanicPutChar(char c);
void SerialPanicPuts(const char* str);
// Minimal printf-like: supports %s %d %u %x %lu %lx %ld %p %c %%
extern "C" void SerialPrintf(const char* fmt, ...);
extern "C" void SerialVPrintf(const char* fmt, __builtin_va_list args);

// Every byte accepted by SerialPutChar is also kept in a 64 KiB in-memory
// tail, so machines without a usable UART (the Dell) still retain recent
// serial diagnostics for the last-log record. Lock-free; a snapshot taken
// while writers lap it may contain a few torn bytes near the oldest end.
uint64_t SerialTailTotal();
// Copy up to capacity newest bytes, oldest first, starting after the first
// newline in the window when one exists. Returns bytes copied (no NUL).
uint32_t SerialTailSnapshot(char* out, uint32_t capacity);

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
