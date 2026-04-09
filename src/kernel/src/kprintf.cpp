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
    SerialPuts(str);
    if (TtyReady()) TtyPuts(str);
}

void KPrintf(const char* fmt, ...)
{
    __builtin_va_list args, args2;
    __builtin_va_start(args, fmt);
    __builtin_va_copy(args2, args);

    SerialVPrintf(fmt, args);
    if (TtyReady()) TtyVPrintf(fmt, args2);

    __builtin_va_end(args2);
    __builtin_va_end(args);
}

} // namespace brook
