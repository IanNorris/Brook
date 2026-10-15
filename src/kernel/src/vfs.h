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

struct VnodeTime {
    int64_t sec;
    int32_t nsec;
};
static constexpr int32_t VFS_UTIME_NOW = 0x3fffffff;
static constexpr int32_t VFS_UTIME_OMIT = 0x3ffffffe;

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
    // Flush this file's buffered data AND its directory entry (size/clusters)
    // to the backing device.  Optional; nullptr if the FS has no per-file
    // sync.  Distinct from the per-mount VfsSync hook: this targets one open
    // file so callers (e.g. the profiler) can make a long-lived file durable
    // mid-write without closing it.
    int (*fsync)  (Vnode* vn);
    // Optional timestamp mutation; times are atime, mtime, with NOW/OMIT.
    int (*set_times)(Vnode* vn, const VnodeTime* times) = nullptr;
    int (*chmod)(Vnode* vn, uint16_t mode) = nullptr;
};

struct Vnode {
    const VnodeOps* ops;
    VnodeType       type;
    void*           priv;     // filesystem-private state (e.g. FIL* or DIR*)
    uint32_t        refCount; // reference count for fork/dup sharing
    uint64_t        cacheId;  // global file identity for page cache; 0 disables caching (not st_ino)
};

struct DirEntry {
    char     name[256];
    uint64_t size;
    bool     isDir;
};

struct VnodeStat {
    uint64_t size;
    uint64_t ino;       // stable inode number (0 = not available)
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;       // Full mode (type + permissions, e.g. S_IFREG | 0644)
    bool     isDir;
    bool     isSymlink;
    VnodeTime atime{};
    VnodeTime mtime{};
    VnodeTime ctime{};
    uint32_t nlink = 1;
    uint64_t dev = 1;    // Linux-encoded filesystem device identity
};

// ---- VFS open flags ----
static constexpr int VFS_O_READ   = 0x00;  // Open for reading (default)
static constexpr int VFS_O_WRITE  = 0x01;  // Open for writing
static constexpr int VFS_O_CREATE = 0x02;  // Create file if it doesn't exist
static constexpr int VFS_O_TRUNC  = 0x04;  // Truncate existing file
static constexpr int VFS_O_APPEND = 0x08;  // Seek to end after open

// ---- Filesystem driver interface ----
//
// A filesystem driver registers a set of callbacks.  The VFS dispatches
// open/stat/unlink/mkdir/rename to the correct driver based on the mount table.
// Each driver is identified by a short name (e.g. "fatfs", "procfs").

struct VfsFsOps {
    // Mount: called when VfsMount() is invoked with this FS name.
    // pdrv is passed through for block-device filesystems (0 for virtual FS).
    // *mountPriv is stored in the mount entry for later use by other callbacks.
    // Return true on success.
    bool (*mount)(uint8_t pdrv, void** mountPriv);

    // Flush and clean up mount-private state. On error retain valid state so
    // the mounted filesystem can retry; return 0 only after successful detach.
    int (*unmount)(void* mountPriv);

    // Open a file or directory.  relPath is relative to the mount point.
    // Returns a Vnode* on success, nullptr on failure.
    Vnode* (*open)(void* mountPriv, uint8_t pdrv, const char* relPath, int flags);

    // Stat by relative path (no open required).  Returns 0 on success.
    int (*stat_path)(void* mountPriv, uint8_t pdrv, const char* relPath, VnodeStat* st);

    // Lstat by relative path — like stat_path but does NOT follow the final symlink.
    // If nullptr, falls back to stat_path.
    int (*lstat_path)(void* mountPriv, uint8_t pdrv, const char* relPath, VnodeStat* st);

    // Delete a file.  Returns 0 on success.
    int (*unlink)(void* mountPriv, uint8_t pdrv, const char* relPath);

    // Create a directory.  Returns 0 on success.
    int (*mkdir)(void* mountPriv, uint8_t pdrv, const char* relPath);

    // Rename.  Returns 0 on success.
    int (*rename)(void* mountPriv, uint8_t pdrv,
                  const char* oldRelPath, const char* newRelPath);

    // Create a symbolic link.  linkRelPath → target.  Returns 0 on success.
    int (*symlink)(void* mountPriv, uint8_t pdrv,
                   const char* target, const char* linkRelPath);

    // Read symlink target.  Writes up to bufsiz bytes into buf.
    // Returns number of bytes written, or <0 on error.
    int (*readlink)(void* mountPriv, uint8_t pdrv,
                    const char* relPath, char* buf, uint64_t bufsiz);

    // Optional: flush dirty in-memory metadata to disk.  May be nullptr
    // for filesystems that always write synchronously.  Called from the
    // VFS sync path (sys_sync / sys_fsync / unmount).
    // Return 0 only after filesystem buffers AND device cache are flushed.
    int (*sync)(void* mountPriv);
    int (*set_times)(void* mountPriv, uint8_t pdrv, const char* relPath,
                     const VnodeTime* times, bool nofollow) = nullptr;
    Vnode* (*open_mode)(void* mountPriv, uint8_t pdrv, const char* relPath,
                       int flags, uint16_t mode) = nullptr;
    int (*chmod)(void* mountPriv, uint8_t pdrv, const char* relPath,
                 uint16_t mode) = nullptr;
    // Check unlink/rmdir intent atomically with the namespace mutation.
    int (*remove_path)(void* mountPriv, uint8_t pdrv, const char* relPath,
                       bool directory) = nullptr;
    bool (*can_unmount)(void* mountPriv) = nullptr;
};

// Register a filesystem driver with the VFS.
// name: short identifier (e.g. "fatfs").  Must remain valid for lifetime of driver.
// ops: pointer to VfsFsOps vtable.  Must remain valid for lifetime of driver.
// Returns true on success.
extern "C" bool VfsRegisterFs(const char* name, const VfsFsOps* ops);

// Unregister a filesystem driver.  Existing mounts using it are NOT affected.
extern "C" bool VfsUnregisterFs(const char* name);

// ---- VFS API ----

// Initialise the VFS subsystem (call once at boot).
extern "C" void VfsInit();

// Mount a filesystem at a path prefix.
// fsName identifies the filesystem type ("fatfs" for now).
// pdrv is the FatFS physical drive number that was bound via FatFsBindDrive().
// mountPoint must start with '/' (e.g. "/", "/boot").
// Returns true on success.
extern "C" bool VfsMount(const char* mountPoint, const char* fsName, uint8_t pdrv);

// Unmount the filesystem at mountPoint. Returns true after successful detach;
// a busy mount or failed filesystem/device flush leaves the mount attached.
extern "C" bool VfsUnmount(const char* mountPoint);

// Enumerate root-level mount points (e.g. "boot" for /boot) so directory
// listings of "/" can expose mounted filesystems even when the backing root
// filesystem does not contain physical mount directories.
uint32_t VfsRootMountCount();
bool VfsRootMountNameAt(uint32_t index, char* out, uint32_t outSize);

// Mount table snapshot for procfs (/proc/mounts).
struct VfsMountSnapshot {
    char mountPoint[64];
    char fsType[16];
    bool used;
};
uint32_t VfsMountMaxSlots();
VfsMountSnapshot VfsMountSnapshotAt(uint32_t idx);

// Filesystem driver registry snapshot for /proc/filesystems
struct VfsFsSnapshot { bool used; char name[16]; };
uint32_t VfsFsMaxSlots();
VfsFsSnapshot VfsFsSnapshotAt(uint32_t idx);

// Open a file or directory.  Caller owns the returned Vnode*; call VfsClose().
// Returns nullptr on error.
extern "C" Vnode* VfsOpen(const char* path, int flags = 0);
// Creation mode is used only for a new inode; the filesystem applies umask.
Vnode* VfsOpenMode(const char* path, int flags, uint16_t mode);

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

// Lstat by path — like VfsStatPath but does not follow the final symlink.
extern "C" int VfsLstatPath(const char* path, VnodeStat* st);

int VfsSetTimes(Vnode* vn, const VnodeTime* times);
int VfsSetTimesPath(const char* path, const VnodeTime* times, bool nofollow);
int VfsChmod(Vnode* vn, uint16_t mode);
int VfsChmodPath(const char* path, uint16_t mode);

// Delete a file by path.
int VfsUnlink(const char* path);
int VfsRemovePath(const char* path, bool directory);

// Create a directory.
int VfsMkdir(const char* path);

// Rename a file or directory.
int VfsRename(const char* oldPath, const char* newPath);

// Create a symbolic link: linkPath → target.
int VfsSymlink(const char* target, const char* linkPath);

// Read symlink target.  Returns bytes written to buf, or <0 on error.
int VfsReadlink(const char* path, char* buf, uint64_t bufsiz);

// Close and free a vnode.
extern "C" void VfsClose(Vnode* vn);

// Flush one vnode, or all mounted filesystems when vn is nullptr. Return the
// first error while still attempting the other mounts.
int VfsSync(Vnode* vn);

// Flush a single open file's data + directory entry to the backing device.
// Filesystems may synchronize the whole mount to satisfy this request.
// Returns 0 on completed flush, negative errno on failure or unsupported sync.
int VfsFsync(Vnode* vn);

} // namespace brook
