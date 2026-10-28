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
}
