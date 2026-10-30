#pragma once
#include <stdint.h>
namespace brook {
struct Device;
struct VfsFsOps;
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
