#pragma once

#include "vfs.h"
#include "proc_wait_info.h"

namespace brook {

// Initialise procfs.  Call once during boot, before VfsMount("/proc", "procfs", 0).
void ProcFsInit();
int ProcFsRootObject(void*,uint8_t,VfsObject**);
int ProcFsVnodeObject(Vnode*,VfsObject**);

// ProcFS VnodeOps — used by the VFS layer when a path falls under /proc.
// Open a procfs path.  relPath is relative to /proc (e.g. "meminfo", "1/stat").
Vnode* ProcFsOpen(const char* relPath, int flags);

// Stat a procfs path without opening it.
int ProcFsStatPath(const char* relPath, VnodeStat* st);

// Open the /proc directory itself for readdir.
Vnode* ProcFsOpenDir(const char* relPath);

// What a task is waiting for, derived from a scheduler snapshot (shared by
// /proc/<pid>/wchan and the `proc blocked` debug command).
struct ProcessSnapshot;
struct PidNamespace;
struct ProcMountView { const PidNamespace* pid; };
struct ProcWaitDescription {
    const char* kind;          // "running", "futex", "poll", "pipe", "gpu-wait", ...
    int fd;                    // fd argument of the current syscall, or -1
    int fdType;                // FdType of that fd, or -1
    uint64_t blockedForMs;     // 0 unless blocked
    char sites[ProcWaitSiteDepth][48]; // kernel wait sites, innermost first
    uint32_t siteCount;
};
void ProcDescribeWait(const ProcessSnapshot& snap, ProcWaitDescription* out);

} // namespace brook
