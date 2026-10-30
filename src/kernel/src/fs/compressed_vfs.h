#pragma once
#include <stdint.h>
namespace brook {
struct Device;
struct VfsFsOps;
// Finalized during serialized boot, before user scheduling; immutable afterward.
// This reports actual RAM publication, never merely an initrd marker's presence.
enum class AppSnapshotBootError : uint32_t {None,Config,Source,Bounds,Memory,Read,Hash,Header,Device,Mount};
struct AppSnapshotBootInfo {
    bool configured=false,sourceClosed=false,ready=false;
    AppSnapshotBootError error=AppSnapshotBootError::None;
    uint64_t bytes=0;
    char source[64]{};
    uint8_t hash[32]{};
};
AppSnapshotBootInfo CompressedVfsAppSnapshotInfo();
const VfsFsOps* CompressedVfsOperations();
// Boot/test binding only; refuse replacing any still-live mounted backing.
bool CompressedVfsBindDevice(uint8_t drive,Device* device);
void CompressedVfsRegister();
void CompressedVfsBootMount();
// Serialized boot-time setup only. Requires an intrinsically read-only FAT source.
bool CompressedVfsMountFile(const char* sourcePath,const char* targetPath);
bool CompressedVfsCleanupPending();
void CompressedVfsCollectRetired();
}
