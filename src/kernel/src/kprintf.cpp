#include "kprintf.h"
#include "serial.h"
#include "tty.h"
#include "debug_overlay.h"

namespace brook {

void KPrintfInit()
{
    DebugOverlayInit();
}

void KPuts(const char* str)
{
    SerialLock();
    if (str) { const char* p = str; while (*p) SerialPutChar(*p++); }
    if (TtyReady()) TtyPuts(str);
    SerialUnlock();

    // Feed the compositor debug overlay (separate lock, safe outside serial lock).
    DebugOverlayPuts(str);
}

// Minimal buffer-based printf for the debug overlay.
// Supports: %s %d %u %x %lx %lu %ld %p %c %% with optional width/zero-pad.
static int OverlayVSprintf(char* buf, int bufSize, const char* fmt, __builtin_va_list args)
{
    int pos = 0;
    auto put = [&](char c) { if (pos < bufSize - 1) buf[pos++] = c; };

    while (*fmt && pos < bufSize - 1)
    {
        if (*fmt != '%') { put(*fmt++); continue; }
        fmt++;
        if (*fmt == '\0') break;

        bool zeroPad = false;
        if (*fmt == '0') { zeroPad = true; fmt++; }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        bool isLong = false;
        if (*fmt == 'l') { isLong = true; fmt++; }

        char tmp[24];
        int tLen = 0;

        switch (*fmt)
        {
        case '%': put('%'); break;
        case 'c': put(static_cast<char>(__builtin_va_arg(args, int))); break;
        case 's': {
            const char* s = __builtin_va_arg(args, const char*);
            if (!s) s = "(null)";
            while (*s && pos < bufSize - 1) put(*s++);
            break;
        }
        case 'd': case 'i': {
            long val = isLong ? __builtin_va_arg(args, long)
                              : static_cast<long>(__builtin_va_arg(args, int));
            bool neg = val < 0;
            unsigned long u = neg ? -static_cast<unsigned long>(val) : static_cast<unsigned long>(val);
            do { tmp[tLen++] = '0' + (u % 10); u /= 10; } while (u);
            if (neg) tmp[tLen++] = '-';
            for (int i = tLen; i < width; i++) put(zeroPad ? '0' : ' ');
            for (int i = tLen - 1; i >= 0; i--) put(tmp[i]);
            break;
        }
        case 'u': {
            unsigned long val = isLong ? __builtin_va_arg(args, unsigned long)
                                       : __builtin_va_arg(args, unsigned int);
            do { tmp[tLen++] = '0' + (val % 10); val /= 10; } while (val);
            for (int i = tLen; i < width; i++) put(zeroPad ? '0' : ' ');
            for (int i = tLen - 1; i >= 0; i--) put(tmp[i]);
            break;
        }
        case 'x': case 'X': case 'p': {
            unsigned long val;
            if (*fmt == 'p') val = reinterpret_cast<unsigned long>(__builtin_va_arg(args, void*));
            else val = isLong ? __builtin_va_arg(args, unsigned long)
                              : __builtin_va_arg(args, unsigned int);
            const char* hex = (*fmt == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            do { tmp[tLen++] = hex[val & 0xF]; val >>= 4; } while (val);
            for (int i = tLen; i < width; i++) put(zeroPad ? '0' : ' ');
            for (int i = tLen - 1; i >= 0; i--) put(tmp[i]);
            break;
        }
        default: put('%'); put(*fmt); break;
        }
        fmt++;
    }
    buf[pos] = '\0';
    return pos;
}

void KPrintf(const char* fmt, ...)
{
    __builtin_va_list args, args2, args3;
    __builtin_va_start(args, fmt);
    __builtin_va_copy(args2, args);
    __builtin_va_copy(args3, args);

    SerialLock();
    SerialVPrintf(fmt, args);
    if (TtyReady()) TtyVPrintf(fmt, args2);
    SerialUnlock();

    // Format into a buffer for the debug overlay.
    char buf[256];
    int len = OverlayVSprintf(buf, sizeof(buf), fmt, args3);
    if (len > 0)
        DebugOverlayPuts(buf);

    __builtin_va_end(args3);
    __builtin_va_end(args2);
    __builtin_va_end(args);
}

} // namespace brook
