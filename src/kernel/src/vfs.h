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
struct FileObject;
struct DirEntry;
struct VnodeStat;
struct VfsObject;
// Mount accounting, in blockSize units. Callbacks fill a kernel buffer only.
struct VfsSpace {
    uint64_t type, blockSize, blocks, freeBlocks, availableBlocks;
    uint64_t files, freeFiles, identity, maxName, fragmentSize, flags;
};

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
    // Resize a regular file without changing any open-file-description offset.
    int (*truncate)(Vnode* vn, uint64_t size) = nullptr;
    // Borrowed stable live-inode object; caller retains vn through use.
    FileObject* (*file_object)(Vnode* vn) = nullptr;
    int (*chown)(Vnode* vn, uint32_t uid, uint32_t gid) = nullptr;
    // Kernel-staged buffer; choose current EOF atomically with the write.
    int (*append)(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset) = nullptr;
    int (*space)(Vnode* vn, VfsSpace* out) = nullptr;
    // Permission inquiry for a retained descriptor, independent of its path.
    int (*access)(Vnode* vn, uint8_t required, bool realIds) = nullptr;
    // Resolve a retained directory's current filesystem-relative name.
    // Optional: legacy backends use the canonical opening path instead.
    int (*directory_path)(Vnode* vn, char* out, uint32_t capacity) = nullptr;
};

struct Vnode {
    const VnodeOps* ops;
    VnodeType       type;
    void*           priv;     // filesystem-private state (e.g. FIL* or DIR*)
    uint32_t        refCount; // reference count for fork/dup sharing
    uint64_t        cacheId;  // global file identity for page cache; 0 disables caching (not st_ino)
    char*           directoryPath = nullptr; // owned canonical opening path; immutable until final close
    // Opaque retained initial-namespace mount instance. Set by VFS dispatch,
    // not by a backend. Its hold prevents unmount/reuse until final close.
    void*           mountOwner = nullptr;
    // Optional backend identity pin, independent of file/iterator state.
    // Final VFS close releases it after backend close, before the mount hold.
    VfsObject*      backendObject = nullptr;
};

// Caller must already own a live vnode reference. The returned name is for
// reporting/legacy callers, not a capability for subsequent global lookup:
// an overmount can make that name resolve to a different retained object.
int VfsDirectoryPath(Vnode* vn, char* out, uint32_t capacity);

// Retention is not safe acquisition from an unprotected raw pointer.
inline void VfsRetain(Vnode* vn) {
    __atomic_fetch_add(&vn->refCount, 1, __ATOMIC_RELEASE);
}

// Drop an owned reference; final destruction occurs outside caller locks.
void VfsRelease(Vnode* vn);

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
static constexpr int VFS_O_EXCL   = 0x10;  // With CREATE, fail if the directory entry exists
static constexpr int VFS_O_APPEND = 0x08;  // Append intent (FD layer retains it).
// WRITE alone is write-only; retain Linux O_RDWR's read requirement separately.
static constexpr int VFS_O_READWRITE = 0x20;
// Internal metadata-only lookup: retain a vnode without requiring data-read
// permission or preloading contents. Never expose this handle as a readable FD.
static constexpr int VFS_O_METADATA = 0x40;
// Internal executable-image read: authorize execute rather than user data read.
// The retained vnode is consumed by the loader, never returned as a readable FD.
static constexpr int VFS_O_EXEC = 0x80;

// ---- Filesystem driver interface ----
//
// A filesystem driver registers a set of callbacks.  The VFS dispatches
// open/stat/unlink/mkdir/rename to the correct driver based on the mount table.
// Each driver is identified by a short name (e.g. "fatfs", "procfs").

struct VfsFsOps {
    // Mount: called when VfsMount() is invoked with this FS name.
    // pdrv is passed through for block-device filesystems (0 for virtual FS).
    // One backing per live driver/device owns *mountPriv. Additional attachments
    // retain that backing without invoking mount again.
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

    // Create a directory. POSIX-capable backends apply mode and caller umask
    // before publishing the inode, and report EEXIST for an occupied name.
    int (*mkdir)(void* mountPriv, uint8_t pdrv, const char* relPath, uint16_t mode);

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
    int (*chown)(void* mountPriv, uint8_t pdrv, const char* relPath,
                 uint32_t uid, uint32_t gid, bool nofollow) = nullptr;
    // Optional open with a precise error (e.g. EACCES). Checked under the
    // filesystem's own inode lock, rather than a racy stat/open preflight.
    Vnode* (*open_mode_checked)(void* mountPriv, uint8_t pdrv, const char* relPath,
                               int flags, uint16_t mode, int* error) = nullptr;
    // Optional permission policy for already-read metadata. The VFS namespace
    // lock stabilizes path/ownership/mode through the following operation.
    int (*check_access)(void* mountPriv, const VnodeStat* st, uint8_t required) = nullptr;
    // Procfs descriptor links are brokered objects, not filesystem text links.
    // Preserve their existing metadata path; syscall-specific FD operations
    // retain their own lifetime/credential checks.
    bool opaque_symlinks = false;
    int (*check_access_real)(void* mountPriv, const VnodeStat* st, uint8_t required) = nullptr;
    // Publish a pathname socket and return a metadata-only inode pin. Creation
    // applies caller credentials/umask atomically; the pin prevents inode reuse
    // after unlink until its owner closes it. Unsupported filesystems omit it.
    Vnode* (*create_socket)(void* mountPriv, uint8_t pdrv, const char* relPath,
                           uint16_t mode, int* error) = nullptr;
    // Retained backend identities. Optional until the backend implements the
    // object-relative contract; callers must return EOPNOTSUPP if absent.
    int (*root_object)(void* mountPriv, uint8_t pdrv, VfsObject** out) = nullptr;
    int (*vnode_object)(Vnode* vnode, VfsObject** out) = nullptr;
};

// Register a filesystem driver with the VFS.
// name: short identifier (e.g. "fatfs").  Must remain valid for lifetime of driver.
// ops: pointer to VfsFsOps vtable.  Must remain valid for lifetime of driver.
// Registration permanently retains callback/data owner modules until reboot.
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
// Open an executable and return the initial fully resolved pathname used for
// the backend open. No truncation; this does not track later rename/unlink.
int VfsOpenExecutable(const char* path, Vnode** vnode, char* resolved, uint32_t size);
// Canonical directory lookup with search permission on the final directory.
int VfsResolveDirectory(const char* path, char* resolved, uint32_t size);
int VfsAccessPath(const char* path, uint8_t required, bool realIds, bool nofollow = false);
int VfsAccess(Vnode* vn, uint8_t required, bool realIds);
// Creation mode is used only for a new inode; the filesystem applies umask.
Vnode* VfsOpenMode(const char* path, int flags, uint16_t mode);
// Reports exclusive-create EEXIST without changing the legacy open ABI.
Vnode* VfsOpenModeChecked(const char* path, int flags, uint16_t mode, int* error);

// Read from a file vnode.  Wrapper around vn->ops->read.
extern "C" int VfsRead(Vnode* vn, void* buf, uint64_t len, uint64_t* offset);

// Lazy file-mapping backing lifetime and mutation gates.
int VfsFileMapPin(Vnode* vn);
void VfsFileMapClone(Vnode* vn);
void VfsFileMapUnpin(Vnode* vn);
extern "C" int VfsTruncate(Vnode* vn, uint64_t size);
extern "C" int VfsWrite(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset);
// Requires a kernel-staged buffer, not a userspace pointer.
extern "C" int VfsAppend(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset);

// Read directory entries.  *cookie starts at 0.
extern "C" int VfsReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie);

// Stat a vnode.
int VfsStat(Vnode* vn, VnodeStat* st);
int VfsSpaceInfo(Vnode* vn, VfsSpace* out);

// Stat by path — uses directory metadata only, does NOT open the file.
extern "C" int VfsStatPath(const char* path, VnodeStat* st);

// Lstat by path — like VfsStatPath but does not follow the final symlink.
extern "C" int VfsLstatPath(const char* path, VnodeStat* st);

int VfsSetTimes(Vnode* vn, const VnodeTime* times);
int VfsSetTimesPath(const char* path, const VnodeTime* times, bool nofollow);
int VfsChmod(Vnode* vn, uint16_t mode);
int VfsChmodPath(const char* path, uint16_t mode);
int VfsChown(Vnode* vn, uint32_t uid, uint32_t gid);
int VfsChownPath(const char* path, uint32_t uid, uint32_t gid, bool nofollow = false);

// Delete a file by path.
int VfsUnlink(const char* path);
int VfsRemovePath(const char* path, bool directory);

// Create a directory.
int VfsMkdir(const char* path, uint16_t mode = 0777);
// Exclusive pathname socket creation. Existing entries report EADDRINUSE.
// The returned metadata-only vnode is owned by the caller, including on unlink.
Vnode* VfsCreateSocket(const char* path, uint16_t mode, int* error);

// Rename a file or directory.
int VfsRename(const char* oldPath, const char* newPath);

// Create a symbolic link: linkPath → target.
int VfsSymlink(const char* target, const char* linkPath);

// Read symlink target.  Returns bytes written to buf, or <0 on error.
int VfsReadlink(const char* path, char* buf, uint64_t bufsiz);

// Close and free a vnode.
// Final destruction only: no other reference may remain. Use VfsRelease to
// drop an owned reference when the vnode can be shared. Backend destruction
// and the final mount hold release happen here, outside caller spinlocks.
extern "C" void VfsClose(Vnode* vn);

// Flush one vnode, or all mounted filesystems when vn is nullptr. Return the
// first error while still attempting the other mounts.
int VfsSync(Vnode* vn);

// Flush a single open file's data + directory entry to the backing device.
// Filesystems may synchronize the whole mount to satisfy this request.
// Returns 0 on completed flush, negative errno on failure or unsupported sync.
int VfsFsync(Vnode* vn);

} // namespace brook
