#pragma once

// Unified kernel print — fans output to both serial and TTY (once TtyReady()).
// Call KPrintfInit() once SerialInit() has been called; TTY output is added
// automatically after TtyInit() succeeds (checked via TtyReady() each call).
//
// KPrintf/KPuts are safe to call at any point after KPrintfInit().
// They are NOT re-entrant; do not call from interrupt handlers.

namespace brook {

// Must be called once SerialInit() has been called.  No-op after that.
void KPrintfInit();

// Output a null-terminated string to serial + TTY.
void KPuts(const char* str);

// printf-style output to serial + TTY.
// Supports: %s %d %u %x %lu %lx %ld %p %c %%
void KPrintf(const char* fmt, ...);

} // namespace brook
