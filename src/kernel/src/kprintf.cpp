#include "kprintf.h"
#include "serial.h"
#include "tty.h"

namespace brook {

void KPrintfInit()
{
    // Serial must already be initialised by the caller.
    // This is a no-op for now; it exists as an explicit lifecycle marker.
}

void KPuts(const char* str)
{
    SerialLock();
    // Call SerialVPrintf indirectly — just write chars directly since we hold the lock.
    if (str) { const char* p = str; while (*p) SerialPutChar(*p++); }
    if (TtyReady()) TtyPuts(str);
    SerialUnlock();
}

void KPrintf(const char* fmt, ...)
{
    __builtin_va_list args, args2;
    __builtin_va_start(args, fmt);
    __builtin_va_copy(args2, args);

    SerialLock();
    SerialVPrintf(fmt, args);
    if (TtyReady()) TtyVPrintf(fmt, args2);
    SerialUnlock();

    __builtin_va_end(args2);
    __builtin_va_end(args);
}

} // namespace brook
