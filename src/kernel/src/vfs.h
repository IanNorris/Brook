#pragma once

#include <stdint.h>

// Minimal VFS — a thin abstraction over mounted filesystems.
//
// Supports open/read/write/readdir/close/stat on a path-based namespace.
// A mount table maps path prefixes to (filesystem, device) pairs.
//
// This is NOT a POSIX fd layer — that comes later when processes exist.
// Kernel code calls VfsOpen() and gets back a Vnode* it owns.

namespace brook {

// ---- Vnode types ----
enum class VnodeType : uint8_t { File, Dir, Device };

struct Vnode;
struct DirEntry;
struct VnodeStat;

// ---- Vnode operations vtable ----
struct VnodeOps {
    // Open the vnode; flags are filesystem-defined (e.g. O_RDONLY=0, O_RDWR=2).
    int (*open)   (Vnode* vn, int flags);
    // Read up to len bytes at *offset; advances *offset.  Returns bytes read or <0.
    int (*read)   (Vnode* vn, void* buf, uint64_t len, uint64_t* offset);
    // Write len bytes at *offset; advances *offset.  Returns bytes written or <0.
    int (*write)  (Vnode* vn, const void* buf, uint64_t len, uint64_t* offset);
    // Read next directory entry; *cookie is opaque iteration state (start at 0).
    // Returns 1 if entry filled, 0 at end-of-dir, <0 on error.
    int (*readdir)(Vnode* vn, DirEntry* out, uint32_t* cookie);
    // Release filesystem-private resources; caller frees the Vnode* after this.
    void (*close) (Vnode* vn);
    // Fill stat.  Returns 0 on success.
    int (*stat)   (Vnode* vn, VnodeStat* st);
};

struct Vnode {
    const VnodeOps* ops;
    VnodeType       type;
    void*           priv;  // filesystem-private state (e.g. FIL* or DIR*)
};

struct DirEntry {
    char     name[256];
    uint64_t size;
    bool     isDir;
};

struct VnodeStat {
    uint64_t size;
    bool     isDir;
};

// ---- VFS API ----

// Initialise the VFS subsystem (call once at boot).
extern "C" void VfsInit();

// Mount a filesystem at a path prefix.
// fsName identifies the filesystem type ("fatfs" for now).
// pdrv is the FatFS physical drive number that was bound via FatFsBindDrive().
// mountPoint must start with '/' (e.g. "/", "/boot").
// Returns true on success.
extern "C" bool VfsMount(const char* mountPoint, const char* fsName, uint8_t pdrv);

// Unmount the filesystem at mountPoint.  The mount entry is cleared and
// f_unmount is called.  Returns true if a matching mount was found.
extern "C" bool VfsUnmount(const char* mountPoint);

// Open a file or directory.  Caller owns the returned Vnode*; call VfsClose().
// Returns nullptr on error.
extern "C" Vnode* VfsOpen(const char* path, int flags = 0);

// Read from a file vnode.  Wrapper around vn->ops->read.
extern "C" int VfsRead(Vnode* vn, void* buf, uint64_t len, uint64_t* offset);

// Write to a file vnode.
extern "C" int VfsWrite(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset);

// Read directory entries.  *cookie starts at 0.
extern "C" int VfsReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie);

// Stat a vnode.
int VfsStat(Vnode* vn, VnodeStat* st);

// Stat by path — uses directory metadata only, does NOT open the file.
extern "C" int VfsStatPath(const char* path, VnodeStat* st);

// Close and free a vnode.
extern "C" void VfsClose(Vnode* vn);

} // namespace brook
