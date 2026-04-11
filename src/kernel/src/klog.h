#pragma once

// Kernel file logger — writes structured log entries to /boot/BROOK.LOG.
//
// KLogInit() opens (or creates) the log file on the boot volume.
// After that, KLog() appends timestamped entries.
// KLogSync() flushes pending writes to disk.
// KLogDump() prints the log contents to the TTY.
//
// Thread-safe: uses the VFS spinlock internally.

namespace brook {

// Open or create the log file.  Call after VFS + boot volume are mounted.
// Returns true if the log file was opened successfully.
bool KLogInit();

// Append a log message.  Format string is printf-style.
void KLog(const char* fmt, ...);

// Flush pending writes to disk.
void KLogSync();

// Print the entire log to TTY and serial.
void KLogDump();

// Close the log file (optional — normally left open).
void KLogClose();

} // namespace brook
