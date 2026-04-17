// Kernel file logger — writes structured log entries to /boot/BROOK.LOG.
// Supports log categories (subsystem) and levels (severity) with runtime
// filtering.  Matching entries can optionally be streamed to the TCP debug
// channel for live remote monitoring.

#include "klog.h"
#include "vfs.h"
#include "serial.h"
#include "tty.h"
#include "spinlock.h"
#include "apic.h"
#include "net.h"
#include "debug_overlay.h"

namespace brook {

static Vnode*    g_logFile  = nullptr;
static uint64_t  g_logOffset = 0;
static SpinLock  g_logLock;

// Per-category minimum log level.  Default: Info.
static LogLevel g_catLevels[static_cast<int>(LogCat::Count)] = {
    LogLevel::Info,  // General
    LogLevel::Info,  // Sched
    LogLevel::Info,  // Mem
    LogLevel::Info,  // Vfs
    LogLevel::Info,  // Net
    LogLevel::Info,  // Input
    LogLevel::Info,  // Wm
    LogLevel::Info,  // Term
    LogLevel::Info,  // Syscall
    LogLevel::Info,  // Driver
    LogLevel::Info,  // Prof
};

static volatile bool g_remoteStream = false;

// ---------------------------------------------------------------------------
// Category names
// ---------------------------------------------------------------------------

static const char* const kCatNames[] = {
    "general", "sched", "mem", "vfs", "net",
    "input", "wm", "term", "syscall", "driver", "prof"
};

static const char* const kLevelNames[] = {
    "ERROR", "WARN", "INFO", "DEBUG", "TRACE"
};

const char* LogCatName(LogCat cat)
{
    int idx = static_cast<int>(cat);
    if (idx >= 0 && idx < static_cast<int>(LogCat::Count))
        return kCatNames[idx];
    return "???";
}

// ---------------------------------------------------------------------------
// Filter control
// ---------------------------------------------------------------------------

void KLogSetLevel(LogCat cat, LogLevel level)
{
    int idx = static_cast<int>(cat);
    if (idx >= 0 && idx < static_cast<int>(LogCat::Count))
        g_catLevels[idx] = level;
}

LogLevel KLogGetLevel(LogCat cat)
{
    int idx = static_cast<int>(cat);
    if (idx >= 0 && idx < static_cast<int>(LogCat::Count))
        return g_catLevels[idx];
    return LogLevel::Info;
}

void KLogSetGlobalLevel(LogLevel level)
{
    for (int i = 0; i < static_cast<int>(LogCat::Count); i++)
        g_catLevels[i] = level;
}

void KLogSetRemoteStream(bool enabled) { g_remoteStream = enabled; }
bool KLogRemoteStreamEnabled()         { return g_remoteStream; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Simple uint64 → decimal string.  Returns length written.
static uint32_t U64ToStr(char* buf, uint64_t val)
{
    if (val == 0) { buf[0] = '0'; return 1; }
    char tmp[20];
    uint32_t n = 0;
    while (val > 0) { tmp[n++] = static_cast<char>('0' + (val % 10)); val /= 10; }
    for (uint32_t i = 0; i < n; ++i) buf[i] = tmp[n - 1 - i];
    return n;
}

bool KLogInit()
{
    // Open or create the log file in append mode on the boot volume.
    g_logFile = VfsOpen("/boot/BROOK.LOG",
                         VFS_O_WRITE | VFS_O_CREATE | VFS_O_APPEND);
    if (!g_logFile)
    {
        SerialPuts("KLOG: failed to open /boot/BROOK.LOG\n");
        return false;
    }

    // Write a boot header.
    static const char header[] = "\n=== Brook OS boot log ===\n";
    uint64_t off = g_logOffset;
    VfsWrite(g_logFile, header, sizeof(header) - 1, &off);
    g_logOffset = off;
    VfsSync(g_logFile);

    SerialPuts("KLOG: logging to /boot/BROOK.LOG\n");
    return true;
}

// Minimal vsnprintf-like formatter for the log.
// Supports: %s %d %u %x %lx %lu %ld %p %c %%
static uint32_t FormatLog(char* buf, uint32_t maxLen,
                           const char* fmt, __builtin_va_list args)
{
    uint32_t pos = 0;
    auto putc = [&](char c) { if (pos + 1 < maxLen) buf[pos++] = c; };
    auto puts = [&](const char* s) { while (*s) putc(*s++); };

    while (*fmt && pos + 1 < maxLen)
    {
        if (*fmt != '%') { putc(*fmt++); continue; }
        ++fmt;
        if (*fmt == '\0') break;

        bool isLong = false;
        if (*fmt == 'l') { isLong = true; ++fmt; }

        switch (*fmt)
        {
        case 's': {
            const char* s = __builtin_va_arg(args, const char*);
            puts(s ? s : "(null)");
            break;
        }
        case 'd': {
            long val = isLong ? __builtin_va_arg(args, long)
                              : static_cast<long>(__builtin_va_arg(args, int));
            if (val < 0) { putc('-'); val = -val; }
            uint32_t n = U64ToStr(buf + pos, static_cast<uint64_t>(val));
            pos += n;
            break;
        }
        case 'u': {
            unsigned long val = isLong ? __builtin_va_arg(args, unsigned long)
                                       : static_cast<unsigned long>(__builtin_va_arg(args, unsigned int));
            pos += U64ToStr(buf + pos, val);
            break;
        }
        case 'x': {
            unsigned long val = isLong ? __builtin_va_arg(args, unsigned long)
                                       : static_cast<unsigned long>(__builtin_va_arg(args, unsigned int));
            if (val == 0) { putc('0'); break; }
            char hex[16]; uint32_t n = 0;
            while (val > 0) { int d = static_cast<int>(val & 0xF); hex[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10); val >>= 4; }
            for (uint32_t i = n; i > 0; --i) putc(hex[i-1]);
            break;
        }
        case 'p': {
            unsigned long val = reinterpret_cast<unsigned long>(__builtin_va_arg(args, void*));
            puts("0x");
            for (int shift = 60; shift >= 0; shift -= 4) {
                int d = static_cast<int>((val >> shift) & 0xF);
                putc(static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10));
            }
            break;
        }
        case 'c': putc(static_cast<char>(__builtin_va_arg(args, int))); break;
        case '%': putc('%'); break;
        default: putc('%'); if (isLong) putc('l'); putc(*fmt); break;
        }
        ++fmt;
    }
    buf[pos] = '\0';
    return pos;
}

// Internal: write a formatted log entry with optional category/level prefix.
static void KLogWrite(LogCat cat, LogLevel level, const char* fmt, __builtin_va_list args)
{
    // Filter check
    int catIdx = static_cast<int>(cat);
    if (catIdx >= 0 && catIdx < static_cast<int>(LogCat::Count)) {
        if (level > g_catLevels[catIdx]) return;
    }

    char line[512];
    uint32_t pos = 0;

    // Prefix: "[tick] LEVEL/cat: "
    line[pos++] = '[';
    uint64_t tick = ApicTickCount();
    pos += U64ToStr(line + pos, tick);
    line[pos++] = ']';
    line[pos++] = ' ';

    // Level tag
    const char* lvl = kLevelNames[static_cast<int>(level)];
    while (*lvl && pos + 1 < sizeof(line)) line[pos++] = *lvl++;
    line[pos++] = '/';

    // Category tag
    const char* cn = kCatNames[catIdx];
    while (*cn && pos + 1 < sizeof(line)) line[pos++] = *cn++;
    line[pos++] = ':';
    line[pos++] = ' ';

    pos += FormatLog(line + pos, sizeof(line) - pos - 2, fmt, args);

    line[pos++] = '\n';
    line[pos]   = '\0';

    // Write to disk under lock.
    if (g_logFile) {
        uint64_t flags = SpinLockAcquire(&g_logLock);
        uint64_t off = g_logOffset;
        VfsWrite(g_logFile, line, pos, &off);
        g_logOffset = off;
        SpinLockRelease(&g_logLock, flags);
    }

    // Echo to serial.
    SerialPuts(line);

    // Stream to debug channel if enabled and connected.
    if (g_remoteStream && DebugChannelConnected()) {
        DebugChannelSend(line);
    }
}

void KLog(const char* fmt, ...)
{
    if (!g_logFile) return;

    char line[512];
    uint32_t pos = 0;

    // Prefix: "[tick] "
    line[pos++] = '[';
    uint64_t tick = ApicTickCount();
    pos += U64ToStr(line + pos, tick);
    line[pos++] = ']';
    line[pos++] = ' ';

    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    pos += FormatLog(line + pos, sizeof(line) - pos - 2, fmt, args);
    __builtin_va_end(args);

    line[pos++] = '\n';
    line[pos]   = '\0';

    // Write under lock.
    uint64_t flags = SpinLockAcquire(&g_logLock);
    uint64_t off = g_logOffset;
    VfsWrite(g_logFile, line, pos, &off);
    g_logOffset = off;
    SpinLockRelease(&g_logLock, flags);

    // Also echo to serial and debug overlay.
    SerialPuts(line);
    DebugOverlayPuts(line);
}

void KLogCat(LogCat cat, LogLevel level, const char* fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    KLogWrite(cat, level, fmt, args);
    __builtin_va_end(args);
}

void KLogSync()
{
    if (!g_logFile) return;
    uint64_t flags = SpinLockAcquire(&g_logLock);
    VfsSync(g_logFile);
    SpinLockRelease(&g_logLock, flags);
}

void KLogDump()
{
    // Read and display the log file from the start.
    Vnode* rd = VfsOpen("/boot/BROOK.LOG", VFS_O_READ);
    if (!rd) { TtyPuts("KLOG: no log file\n"); return; }

    char buf[256];
    uint64_t off = 0;
    for (;;)
    {
        int n = VfsRead(rd, buf, sizeof(buf) - 1, &off);
        if (n <= 0) break;
        buf[n] = '\0';
        TtyPuts(buf);
        SerialPuts(buf);
    }
    VfsClose(rd);
}

void KLogClose()
{
    if (g_logFile)
    {
        VfsSync(g_logFile);
        VfsClose(g_logFile);
        g_logFile = nullptr;
    }
}

} // namespace brook
