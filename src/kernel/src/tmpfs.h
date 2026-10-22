#pragma once
#include <stdint.h>
namespace brook
{
struct VfsFsOps;
struct UserNamespace;
// The prepared instance remains caller-owned until MountFresh succeeds.
struct Tmpfs;
const VfsFsOps *TmpfsOperations();
Tmpfs *TmpfsCreate(uint32_t uid, uint32_t gid, UserNamespace *owner, uint64_t flags, int *error);
int TmpfsDestroy(Tmpfs *);
struct TmpfsUsage
{
    uint64_t bytes, inodes, mounts;
};
TmpfsUsage TmpfsGlobalUsage();
} // namespace brook
