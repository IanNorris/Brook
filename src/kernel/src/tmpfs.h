#pragma once
#include <stdint.h>
namespace brook
{
struct VfsFsOps;
struct UserNamespace;
// The prepared instance remains caller-owned until MountFresh succeeds.
struct Tmpfs;
inline constexpr uint64_t TmpfsDefaultBytes = 16ULL * 1024 * 1024;
// Boot-time aggregate charged-memory budget. Configuration is rejected while
// any instance exists; allocation remains lazy and each mount has its own quota.
int TmpfsConfigureByteBudget(uint64_t bytes);
uint64_t TmpfsMaximumBytes();
const VfsFsOps *TmpfsOperations();
Tmpfs *TmpfsCreate(uint32_t uid, uint32_t gid, UserNamespace *owner, uint64_t flags, int *error,
                   uint16_t rootMode=01777, uint64_t byteLimit=TmpfsDefaultBytes);
int TmpfsDestroy(Tmpfs *);
struct TmpfsUsage
{
    uint64_t bytes, inodes, mounts;
};
TmpfsUsage TmpfsGlobalUsage();
} // namespace brook
