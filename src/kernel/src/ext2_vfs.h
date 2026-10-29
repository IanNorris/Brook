#pragma once

// ext2_vfs.h — Ext2 filesystem driver for Brook VFS.

#include <stdint.h>

namespace brook {

struct Device;

// Cumulative mount-lifetime diagnostics, snapshotted under the filesystem lock.
struct Ext2SyncProbe {
    uint64_t autoCount, fsyncCount, mountCount, errors, waitNs, autoNs, fsyncNs, mountNs;
    uint32_t autoOps, pendingOps;
};
bool Ext2GetSyncProbe(uint8_t pdrv, Ext2SyncProbe& out);
struct Ext2WritebackProbe {
    uint64_t capacity, queued, records, inputs, merges, writes, bytes;
    uint64_t drains, capacityDrains, syncDrains, backgroundDrains, errors;
};
bool Ext2GetWritebackProbe(uint8_t pdrv,Ext2WritebackProbe& out);
void Ext2StartWriteback();
void Ext2StopWriteback();

// Register "ext2" filesystem driver with the VFS.
void Ext2VfsRegister();
// Call once after fw_cfg initialization and before mounting block devices.
void Ext2ConfigureAutoSync();

// Force-release the ext2 lock if held by a dying process.
void Ext2ForceUnlockForPid(uint32_t pid);

// Change inode mode (permissions only, preserves type bits).
// Returns 0 on success, -errno on failure.
int Ext2Chmod(const char* path, uint16_t mode);

// Change inode uid/gid. Pass (uint32_t)-1 to leave unchanged.
int Ext2Chown(const char* path, uint32_t uid, uint32_t gid);

// Mark an ext2 mount (by pdrv) as skipping permission checks.
// Used for /nix — packages must remain world-accessible regardless of user.
void Ext2SetSkipPermChecks(uint8_t pdrv, bool skip);

} // namespace brook

// Bind a block device to an ext2 physical-drive slot (0-based).
// Must be called before VfsMount(..., "ext2", pdrv).
extern "C" bool Ext2BindDevice(uint8_t pdrv, brook::Device* dev);
