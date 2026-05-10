# VFS & Filesystem Subsystem Documentation

## Overview

Brook's Virtual Filesystem (VFS) provides a unified file I/O interface across
multiple filesystem backends: ext2 (primary), FatFS (boot partition), and
ProcFS (/proc). It handles mount management, path resolution, symlink
traversal, and delegates to per-filesystem operations.

## Architecture

```
User syscalls (sys_open, sys_read, sys_write, sys_stat, ...)
  → VFS layer (vfs.cpp): path resolution, mount lookup, symlink resolution
    → Filesystem backends:
        ext2_vfs.cpp  — ext2 on virtio-blk (primary data partition)
        fatfs_vfs.cpp — FatFS on virtio-blk (boot FAT32 partition)
        procfs.cpp    — /proc virtual filesystem
```

## Files

| File | Lines | Purpose |
|------|-------|---------|
| vfs.h | 192 | VFS types, Vnode, VnodeOps, VfsFsOps interfaces |
| vfs.cpp | 575 | VFS core: mount table, path resolution, symlink traversal |
| ext2_vfs.cpp | 2259 | ext2 filesystem implementation |
| ext2_vfs.h | 21 | ext2 public interface |
| fatfs_vfs.cpp | 548 | FatFS VFS adapter with file caching |
| fatfs_vfs.h | 10 | FatFS public interface |
| fatfs_glue.cpp | 198 | FatFS ↔ block device I/O glue |
| fatfs_glue.h | 22 | FatFS glue interface |
| procfs.cpp | 824 | /proc filesystem (cpuinfo, meminfo, stat, pid/*) |
| procfs.h | 20 | ProcFS public interface |
| procfs_vfs.cpp | 53 | ProcFS VFS registration |
| procfs_vfs.h | 9 | ProcFS VFS header |

## Key Data Structures

### Vnode (vfs.h)
```cpp
struct Vnode {
    const VnodeOps* ops;      // filesystem-specific operation table
    VnodeType       type;     // File, Directory, Symlink, etc.
    void*           priv;     // filesystem-private state
    uint32_t        refCount; // NOTE: declared but UNUSED (BRO-114)
    uint64_t        cacheId;  // unique file identity for page cache
};
```

### Mount Table (vfs.cpp)
- `g_mounts[VFS_MAX_MOUNTS=8]` — fixed-size mount table
- Each entry: mountPoint (64 bytes), fsOps, mountPriv, used flag
- Longest-prefix match in `FindMount()`

### Filesystem Driver Registry (vfs.cpp)
- `g_fsDrivers[VFS_MAX_FS_DRIVERS=8]` — registered filesystem types
- Looked up by name string in `VfsMount()`

## Path Resolution Flow

1. `VfsResolveSymlinks(path, resolved, maxLen, followFinal)` — resolves symlinks
2. `NormalizePath(path, out, maxLen)` — removes `.`, `..`, collapses slashes
3. `FindMount(path)` — finds mount with longest matching prefix
4. Filesystem `open(relPath, flags)` — opens file relative to mount

Symlink depth limit: 8 (see BRO-113 for bypass concern).

## Known Issues

### Bug Tickets Filed

| Bug | Severity | Summary |
|-----|----------|---------|
| BRO-114 | High | g_vfsLock declared but never acquired — VFS not thread-safe |
| BRO-111 | High | ext2 directory entry buffer overflow from unchecked rec_len |
| BRO-112 | High | ext2 unchecked block pointers allow out-of-bounds disk I/O |
| BRO-113 | Medium | ext2 symlink depth bypass and rec_len infinite loop |
| BRO-115 | High | FatFS cache placeholder deadlock if CacheFileUnlocked fails |

### Design Gaps

- **Vnode refCount unused**: `VfsClose()` always frees on first close. Fork/dup
  sharing requires reference counting to avoid use-after-free.
- **Error codes inconsistent**: Most functions return -1; some return -EINVAL.
  Should use proper errno values throughout.
- **No per-filesystem locking**: Single global lock (when fixed) will be a
  bottleneck. Consider per-mount locking.
- **Mount point not normalized**: `VfsMount()` doesn't normalize paths before
  storing, so `FindMount()` may not match non-canonical paths.

## ext2 Implementation

### Key Functions

| Function | Purpose |
|----------|---------|
| `Ext2FsMount()` | Parse superblock, load BGDT, init caches |
| `Ext2FsOpen()` | Resolve path → inode → Vnode |
| `Ext2FileRead()` / `Write()` | Data I/O via block mapping |
| `Ext2BlockMap()` | Logical block → physical block (direct/indirect/double) |
| `Ext2EnsureBlock()` | Allocate blocks on write |
| `Ext2AllocBlock()` / `FreeBlock()` | Bitmap-based block allocation |
| `Ext2AllocInode()` / `FreeInode()` | Bitmap-based inode allocation |
| `Ext2DirLookup()` | Search directory for name |
| `Ext2DirAdd()` / `Remove()` | Directory entry management |
| `Ext2ResolvePath()` | Full path → inode with symlink resolution |
| `Ext2Sync()` | Flush dirty bitmaps and superblock |

### Caching
- **Indirect block pointer cache**: per-mount, spinlock-protected
- **Block bitmap cache**: lazy-loaded per group, dirty-tracked
- **Inode cache**: fixed-size array with LRU eviction

### Disk Data Trust Issues
- Block pointers not validated against totalBlocks (BRO-112)
- Directory entry fields (rec_len, name_len) not bounds-checked (BRO-111)
- Superblock fields (blocks_per_group, inodes_per_group) not zero-checked
- Triply-indirect writes not implemented (large files may fail to grow)

### Concurrency
- `KRwLock` per-mount: readers for read ops, writer for metadata changes
- Indirect cache spinlock held during disk I/O (latency concern)

## FatFS Implementation

### File Caching
- Large read-only files (>64KB) cached in kernel memory
- Cache uses placeholder+loading pattern (deadlock risk — BRO-115)
- `g_fatLock` KMutex serializes all FatFS operations

### Disk I/O Glue (fatfs_glue.cpp)
- `disk_read()`/`disk_write()` convert sector+count to byte offset
- No integer overflow checks on sector arithmetic
- `FatFsDeviceReady()` validates device registration and ops pointers

## ProcFS Implementation

### Generated Files
| Path | Content |
|------|---------|
| `/proc/cpuinfo` | CPU model, features, cache sizes |
| `/proc/meminfo` | Physical/free/cached memory stats |
| `/proc/stat` | Per-CPU tick counters |
| `/proc/uptime` | System uptime |
| `/proc/loadavg` | Stubbed: "0.00 0.00 0.00" |
| `/proc/[pid]/stat` | Process state, ticks, memory |
| `/proc/[pid]/status` | Human-readable process info |
| `/proc/[pid]/maps` | Memory map (leaks kernel addresses — BRO-117) |
| `/proc/[pid]/cmdline` | Process command line |

### Security Notes
- No permission checks — any process can read any /proc/[pid]/*
- Kernel pointers exposed in maps file
- Acceptable for hobby OS; needs hardening for production

## Audit History

- **2026-05-10**: Full audit of VFS core, ext2, FatFS, ProcFS.
  Filed BRO-111 through BRO-117.
