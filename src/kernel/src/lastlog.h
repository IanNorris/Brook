#pragma once
// lastlog.h -- periodic on-disk copy of the kernel log tail and compat census
// (see lastlog.cpp and lastlog_format.h).
namespace brook {
// Boot only, after the scheduler and VFS exist. The thread waits for the
// persistent /nix ext2 store and does nothing until it is mounted.
void LastLogStart();
// Write a record at the next poll (once the store is mounted).
void LastLogRequestFlush();
// Debug channel: "lastlog status|on|off|flush|previous OFFSET COUNT".
// Returns false when the command is not a lastlog command.
bool LastLogHandleCommand(const char* cmd);
}
