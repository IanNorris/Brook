#pragma once
#include <stdint.h>

// FatFS VFS driver — registers "fatfs" filesystem with the VFS layer.
// Call FatFsVfsRegister() at boot before any VfsMount("...", "fatfs", ...).

namespace brook {

struct Vnode;
struct FatImageLease;
struct FatPrivateMount;
struct Device;
#if defined(BROOK_SSD_GUARD_CHECK)
struct GuardFatProfile;
// Diagnostic only: reserves an exclusive read-only FAT slot for this boot,
// verifies mounted geometry and reads both fixed records without publishing
// a mount or granting recovery/write authority. Outputs are cleared on error.
int FatFsGuardInspectReadOnly(uint8_t pdrv, Device* device, const GuardFatProfile& profile,
                              uint8_t (&records)[2][128]);
#if defined(BROOK_SSD_GUARD_TRANSACTION)
bool FatFsPrivateRecoveryGeometryMatches(FatPrivateMount* mount,const GuardFatProfile& profile);
// 0 rejected, 1 uncertain, 2 pending records durably verified. One shot;
// never grants data-partition or normal boot authority.
int FatFsPrivateRecoveryTestPending(FatPrivateMount* mount,const char* identity,uint32_t failAt);
#if defined(BROOK_SSD_GUARD_COMPLETION)
int FatFsPrivateRecoveryTestComplete(FatPrivateMount* mount,const char* identity,const uint8_t* pending,uint32_t failAt);
#endif
#endif
#endif
void FatFsVfsRegister();
// Flush dirty ordinary FAT mounts, retaining failures for the next pass.
int FatFsBackgroundSync();
// Boot-only, one-shot mount on a pinned exclusive binding. It is not inserted
// in a VFS namespace and, once bound, remains reserved until reboot even if
// mounting fails.
// This does not by itself authorize recovery writes.
bool FatFsMountPrivateRecovery(uint8_t pdrv, Device* expected, FatPrivateMount** out);
// One-shot, read-only inspection of the two fixed guard files. Successful or
// failed partial preparation remains pinned for the boot; output buffers may
// be partially filled on failure. No write authority or namespace entry is
// exposed. Success does not validate record contents or third-file crosslinks.
int FatFsPrivateRecoveryInspect(FatPrivateMount* mount, void* first128, void* second128);
#if defined(BROOK_SSD_GUARD_PENDING_TEST)
// Disposable test only: retain a checked fixed journal on the private mount.
int FatFsPrivateRecoveryPinJournal(FatPrivateMount* mount);
int FatFsPrivateRecoveryPublishJournalTest(FatPrivateMount* mount);
#endif
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
// Both live leases must be retained without concurrent release. Checks that
// their identities and one-cluster allocations differ on the same FAT mount;
// does not establish exclusive ownership of the rest of the mount.
int FatFsGuardPairValidate(FatImageLease* first, FatImageLease* second);
// Copy the fixed journal file's sector map relative to its bound device. The
// lease stays pinned. This does not rule out third-file crosslinks or grant
// raw-write authority.
int FatFsJournalSectorMap(FatImageLease* lease, uint64_t* out, uint32_t sectors);
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
