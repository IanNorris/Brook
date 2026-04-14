#pragma once

#include "vfs.h"

namespace brook {

// Initialise procfs.  Call once during boot, before VfsMount("/proc", "procfs", 0).
void ProcFsInit();

// ProcFS VnodeOps — used by the VFS layer when a path falls under /proc.
// Open a procfs path.  relPath is relative to /proc (e.g. "meminfo", "1/stat").
Vnode* ProcFsOpen(const char* relPath, int flags);

// Stat a procfs path without opening it.
int ProcFsStatPath(const char* relPath, VnodeStat* st);

// Open the /proc directory itself for readdir.
Vnode* ProcFsOpenDir(const char* relPath);

} // namespace brook
