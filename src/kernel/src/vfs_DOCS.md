# VFS Subsystem — Brook OS

## Overview

The Virtual File System (VFS) provides a unified path-based namespace over multiple
filesystem implementations. Kernel code and syscalls operate on Vnodes — opaque handles
with a vtable for read/write/readdir/stat operations. The VFS dispatches operations to
the correct filesystem driver based on mount-point prefix matching.

**Files:**
| File | Lines | Purpose |
|------|-------|---------|
| `vfs.h` | ~190 | Vnode/VnodeOps/VfsFsOps structs, public API |
| `vfs.cpp` | ~575 | Mount table, path resolution, dispatch layer |
| `ext2_vfs.cpp` | ~2440 | Ext2 read/write filesystem driver |
| `ext2_vfs.h` | ~20 | Ext2 driver registration and globals |
| `fatfs_vfs.cpp` | ~570 | FAT32 filesystem driver (boot partition) |
| `procfs.cpp` | ~1290 | /proc virtual filesystem |
| `procfs_vfs.cpp` | ~50 | Procfs VfsFsOps adapter |

## Architecture

```
┌──────────────┐
│  Syscalls    │  open/read/write/close/stat/readdir/unlink/mkdir/rename
│  (syscall.cpp)│
└──────┬───────┘
       │  VfsOpen(path) / VfsRead(vn,...) / etc.
┌──────┴───────┐
│  VFS Layer   │  Mount table (8 entries max), longest-prefix matching
│  (vfs.cpp)   │  Path → MountEntry → relPath → driver dispatch
└──────┬───────┘
       │  VfsFsOps vtable callbacks
┌──────┼───────────────┬───────────────┐
│  ext2_vfs    │  fatfs_vfs    │  procfs_vfs   │
│  /nix, /home │  /boot (ESP)  │  /proc        │
│  Read/Write  │  Read/Write   │  Read-only     │
│  Symlinks    │  FAT32+LFN    │  Dynamic gen   │
└──────────────┴───────────────┴───────────────┘
       │
┌──────┴───────┐
│  Block Layer │  virtio-blk / ramdisk
└──────────────┘
```

## Mount Table

- Fixed 8-entry table (`VFS_MAX_MOUNTS`)
- Each entry: mount point string, filesystem name, drive number, VfsFsOps*, mount-private data
- **Longest-prefix matching**: `/nix/store/foo` matches mount at `/nix` over `/`
- Mount/unmount at runtime via `VfsMount()` / `VfsUnmount()`

### Typical Mount Layout
| Mount Point | Filesystem | Device | Purpose |
|-------------|-----------|--------|---------|
| `/boot` | fatfs | drive 0 | ESP (UEFI boot partition) |
| `/nix` | ext2 | drive 1 | Nix store + closures |
| `/proc` | procfs | — | Virtual process info |
| `/home` | ext2 | drive 1 | User home directory |

## Vnode Model

```c
struct Vnode {
    const VnodeOps* ops;    // vtable: open/read/write/readdir/close/stat
    VnodeType       type;   // File, Dir, Device
    void*           priv;   // FS-private (e.g. ext2 inode state, FIL*)
    uint32_t        refCount; // for fork/dup sharing
    uint64_t        cacheId;  // unique file identity for block cache dedup
};
```

Callers own the Vnode pointer — call `VfsClose()` to release it. Fork/dup
increment `refCount`; the vnode is freed only when refCount reaches 0.

## Filesystem Driver Interface (VfsFsOps)

Each driver registers via `VfsRegisterFs(name, ops)`:

| Callback | Purpose |
|----------|---------|
| `mount(pdrv, mountPriv)` | Initialize driver state for this mount |
| `unmount(mountPriv)` | Clean up driver state |
| `open(mountPriv, pdrv, relPath, flags)` | Open file/directory → Vnode* |
| `stat_path(mountPriv, pdrv, relPath, st)` | Stat without opening |
| `lstat_path(...)` | Stat without following final symlink |
| `unlink(mountPriv, pdrv, relPath)` | Delete a file |
| `mkdir(mountPriv, pdrv, relPath)` | Create a directory |
| `rename(mountPriv, pdrv, old, new)` | Rename/move |
| `symlink(mountPriv, pdrv, target, link)` | Create symbolic link |
| `readlink(mountPriv, pdrv, path, buf, sz)` | Read symlink target |
| `sync(mountPriv)` | Flush dirty metadata to disk |

## Open Flags

| Flag | Value | Meaning |
|------|-------|---------|
| `VFS_O_READ` | 0x00 | Read-only (default) |
| `VFS_O_WRITE` | 0x01 | Write access |
| `VFS_O_CREATE` | 0x02 | Create if not exists |
| `VFS_O_TRUNC` | 0x04 | Truncate existing file |
| `VFS_O_APPEND` | 0x08 | Seek to end after open |

## Ext2 Driver

The ext2 driver provides full read/write support for Linux ext2 filesystems.

### Features
- Superblock parsing and block group descriptor table
- Inode lookup with directory traversal
- Block mapping: direct (0–11), indirect, doubly-indirect, triply-indirect
- Block/inode allocation from bitmaps (first-fit within group)
- File read and write (extending files, allocating new blocks)
- Directory operations: create entries, remove entries, rename
- Symbolic link resolution (up to 8 levels)
- `mkdir`, `unlink`, `rename`, `symlink`
- Automatic dirty metadata flush via `sync()`

### Locking
- Single `KRwLock` (`g_ext2Lock`) protects all metadata operations
- Read lock for: reads, stat, readdir
- Write lock for: write, unlink, mkdir, rename, symlink, block/inode alloc
- `Ext2ForceUnlockForPid()`: emergency unlock on process death (prevents deadlock)

### Block Cache Integration
- 4096-entry block cache shared across all ext2 mounts
- Entries keyed by (device, blockNumber)
- Write-back: dirty blocks flushed on sync or eviction
- Reduces disk I/O for repeated reads of metadata and small files

### Limitations
- No journaling (ext3/ext4 journal is ignored)
- No extended attributes
- No sparse file support
- Single global lock limits concurrent I/O throughput

## FAT32 Driver

Wraps the FatFS library for FAT32/FAT16 access (typically the UEFI boot partition).

### Features
- FAT32 with long filename (LFN) support
- Read, write, create, delete, mkdir, rename
- Directory iteration via FatFS `f_readdir`

## Procfs Driver

Virtual filesystem generating process and system information on the fly.

### Global Files (`/proc/`)
| File | Content |
|------|---------|
| `stat` | Per-CPU tick counters (user/nice/system/idle), fork count |
| `meminfo` | MemTotal, MemFree, MemAvailable (from PMM) |
| `uptime` | Seconds since boot, idle time (from per-CPU counters) |
| `version` | Kernel version string |
| `loadavg` | Approximate load average |
| `cpuinfo` | Per-CPU model name, MHz, features |
| `modules` | Loaded kernel modules (name, size, state) |
| `mounts` | Mounted filesystems (device, mountpoint, type) |
| `diskstats` | Block device I/O statistics (reads, writes, sectors) |
| `filesystems` | Registered filesystem drivers |
| `net/dev` | Per-interface traffic counters (bytes/packets Rx/Tx) |
| `net/tcp` | TCP socket table (Linux-compatible format) |
| `net/udp` | UDP socket table |
| `net/sockstat` | Socket count summary (TCP, UDP, RAW in-use) |

### Per-PID Files (`/proc/[pid]/`)
| File | Content |
|------|---------|
| `stat` | PID, name, state, ppid, threads, utime, stime, vsize, rss |
| `status` | Human-readable: Name, Pid, PPid, Pgid, Sid, VmSize, VmRSS, Threads |
| `statm` | Memory usage in pages |
| `cmdline` | NUL-terminated command name |
| `maps` | VM map (heap and stack regions) |
| `exe` | Executable path |
| `cwd` | Current working directory |
| `limits` | Resource limits (open files, stack, data, max processes) |
| `fd/` | Directory listing open file descriptors (entries are fd numbers, reading shows target path/type) |

### Special Paths
- `/proc/self` → resolves to current process's PID directory

## Public API

| Function | Description |
|----------|-------------|
| `VfsInit()` | Initialize mount table |
| `VfsMount(point, fsName, pdrv)` | Mount a filesystem |
| `VfsUnmount(point)` | Unmount |
| `VfsOpen(path, flags)` | Open file → Vnode* (caller owns) |
| `VfsRead(vn, buf, len, offset)` | Read from vnode |
| `VfsWrite(vn, buf, len, offset)` | Write to vnode |
| `VfsReaddir(vn, out, cookie)` | Read directory entry |
| `VfsStat(vn, st)` | Stat a vnode |
| `VfsStatPath(path, st)` | Stat by path (no open) |
| `VfsLstatPath(path, st)` | Lstat by path (no follow symlink) |
| `VfsUnlink(path)` | Delete file |
| `VfsMkdir(path)` | Create directory |
| `VfsRename(old, new)` | Rename |
| `VfsSymlink(target, link)` | Create symlink |
| `VfsReadlink(path, buf, sz)` | Read symlink target |
| `VfsClose(vn)` | Release vnode |
| `VfsSync(vn)` | Flush pending writes |

## Known Limitations

1. **Max 8 mounts**: Fixed table size. Adequate for current use.
2. **No inotify/dnotify**: No filesystem change notifications.
3. **No hard links**: Ext2 driver doesn't implement hard link creation.
4. **No file locking**: No flock/fcntl advisory locks.
5. **Single ext2 lock**: All ext2 operations serialize globally. Performance
   impact mitigated by block cache but heavy concurrent I/O will bottleneck.
6. **No tmpfs**: `/tmp` is not memory-backed; writes go to ext2 on disk.

### Security Notes
- No permission checks on /proc — any process can read any `/proc/[pid]/*`
- Kernel pointers exposed in `/proc/[pid]/maps` (acceptable for hobby OS)
