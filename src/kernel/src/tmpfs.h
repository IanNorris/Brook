#pragma once
#include <stdint.h>
namespace brook
{
struct VfsFsOps;
struct UserNamespace;
// The prepared instance remains caller-owned until MountFresh succeeds.
struct Tmpfs;
inline constexpr uint64_t TmpfsDefaultBytes = 16ULL * 1024 * 1024;
// Boot-only, lazy-allocation aggregate limit. The larger profile is an opt-in
// comparison, not a promise that this memory is reserved or reclaimable.
inline uint64_t TmpfsBootBudget(uint64_t freePages, bool largerProfile)
{
    uint64_t pages = freePages / (largerProfile ? 4 : 8);
    constexpr uint64_t maxPages = 0x7ffffffffffff000ULL / 4096;
    if (pages > maxPages) pages = maxPages;
    return pages * 4096;
}
// Diagnostic opt-in can devote more of the existing aggregate budget to the
// desktop mount. This changes no reservation and never enlarges the aggregate.
inline uint64_t TmpfsDesktopBudget(uint64_t aggregateBytes, bool widerDesktop)
{
    const uint64_t pages = aggregateBytes / 4096;
    const uint64_t desktopPages = widerDesktop ? pages - pages / 4 : pages / 2;
    return desktopPages * 4096;
}
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
