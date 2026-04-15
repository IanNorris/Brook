#pragma once

#include <stdint.h>

// Kernel file logger — writes structured log entries to /boot/BROOK.LOG.
//
// KLogInit() opens (or creates) the log file on the boot volume.
// After that, KLog() appends timestamped entries.
// KLogSync() flushes pending writes to disk.
// KLogDump() prints the log contents to the TTY.
//
// Thread-safe: uses the VFS spinlock internally.
//
// Log categories and levels allow filtering output at runtime.
// Remote log streaming sends matching entries to the TCP debug channel.

namespace brook {

// ---------------------------------------------------------------------------
// Log levels (severity)
// ---------------------------------------------------------------------------
enum class LogLevel : uint8_t {
    Error   = 0,    // Critical errors
    Warn    = 1,    // Warnings
    Info    = 2,    // Normal informational messages
    Debug   = 3,    // Verbose debug output
    Trace   = 4,    // Very verbose tracing

    Count   = 5
};

// ---------------------------------------------------------------------------
// Log categories (subsystem)
// ---------------------------------------------------------------------------
enum class LogCat : uint8_t {
    General  = 0,   // Uncategorised / boot messages
    Sched    = 1,   // Scheduler / process management
    Mem      = 2,   // PMM / VMM / heap
    Vfs      = 3,   // VFS / filesystem
    Net      = 4,   // Network stack
    Input    = 5,   // Keyboard / mouse / input
    Wm       = 6,   // Window manager / compositor
    Term     = 7,   // Terminal emulator
    Syscall  = 8,   // Syscall handler
    Driver   = 9,   // Device drivers
    Prof     = 10,  // Profiler

    Count    = 11
};

// Category names (for display / remote protocol).
const char* LogCatName(LogCat cat);

// ---------------------------------------------------------------------------
// Runtime filter control
// ---------------------------------------------------------------------------

// Set the minimum log level for a category.  Messages below this level
// are discarded.  Default: Info for all categories.
void KLogSetLevel(LogCat cat, LogLevel level);

// Get the current level for a category.
LogLevel KLogGetLevel(LogCat cat);

// Set level for ALL categories at once.
void KLogSetGlobalLevel(LogLevel level);

// Enable/disable streaming matching log entries to the debug channel.
void KLogSetRemoteStream(bool enabled);
bool KLogRemoteStreamEnabled();

// ---------------------------------------------------------------------------
// Core logging API
// ---------------------------------------------------------------------------

// Open or create the log file.  Call after VFS + boot volume are mounted.
bool KLogInit();

// Original uncategorised log (treated as General/Info).
void KLog(const char* fmt, ...);

// Categorised + levelled log.
void KLogCat(LogCat cat, LogLevel level, const char* fmt, ...);

// Flush pending writes to disk.
void KLogSync();

// Print the entire log to TTY and serial.
void KLogDump();

// Close the log file (optional — normally left open).
void KLogClose();

// ---------------------------------------------------------------------------
// Convenience macros
// ---------------------------------------------------------------------------
#define KLOG_ERROR(cat, fmt, ...) ::brook::KLogCat(::brook::LogCat::cat, ::brook::LogLevel::Error, fmt, ##__VA_ARGS__)
#define KLOG_WARN(cat, fmt, ...)  ::brook::KLogCat(::brook::LogCat::cat, ::brook::LogLevel::Warn,  fmt, ##__VA_ARGS__)
#define KLOG_INFO(cat, fmt, ...)  ::brook::KLogCat(::brook::LogCat::cat, ::brook::LogLevel::Info,  fmt, ##__VA_ARGS__)
#define KLOG_DEBUG(cat, fmt, ...) ::brook::KLogCat(::brook::LogCat::cat, ::brook::LogLevel::Debug, fmt, ##__VA_ARGS__)
#define KLOG_TRACE(cat, fmt, ...) ::brook::KLogCat(::brook::LogCat::cat, ::brook::LogLevel::Trace, fmt, ##__VA_ARGS__)

} // namespace brook
