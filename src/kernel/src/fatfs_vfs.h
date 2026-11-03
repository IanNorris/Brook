#pragma once
#include <stdint.h>

// FatFS VFS driver — registers "fatfs" filesystem with the VFS layer.
// Call FatFsVfsRegister() at boot before any VfsMount("...", "fatfs", ...).

namespace brook {

struct Vnode;
struct FatImageLease;
struct Device;
void FatFsVfsRegister();
// True only for a streaming regular FIL whose retained drive binding is immutable.
bool FatFsFileSourceReadOnly(Vnode* node);

// Boot-only fixed-image ownership. The caller must privately own the sole
// reference to an uncached read/write FAT vnode. Acquisition retains it and
// rejects all other open handles. The source cannot be reopened, mapped,
// resized or renamed while leased. No path resolution occurs during I/O.
int FatFsAcquireImageLease(Vnode* node, uint64_t bytes, FatImageLease** out);
// Same exclusive retained ownership for one pre-existing 128-byte recovery
// guard. The image API's sector alignment requirements remain unchanged.
int FatFsAcquireGuardLease(Vnode* node, FatImageLease** out);
// Atomically sync the retained guard and read its 128 bytes through diskio,
// bypassing FatFs file/windows. Errors poison the lease and retain ownership.
int FatFsGuardFlushReadback(FatImageLease* lease, void* out);
int FatFsImageRead(FatImageLease* lease, uint64_t offset, void* out, uint32_t bytes);
int FatFsImageWrite(FatImageLease* lease, uint64_t offset, const void* in, uint32_t bytes);
int FatFsImageFlush(FatImageLease* lease);
// Borrowed backing binding while the lease is retained.
Device* FatFsImageLeaseDevice(FatImageLease* lease);
// No concurrent callers or published filesystem/device may remain. On error
// the lease stays valid and pinned; never discard dirty state after failed sync.
// Release performs VFS finalization outside the FAT mutex.
int FatFsReleaseImageLease(FatImageLease* lease);

} // namespace brook
