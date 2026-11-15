#pragma once
#include <stdint.h>

namespace brook {

// Linux new_encode_dev() for the character device numbers seeded below.
constexpr uint64_t DevEncode(uint32_t major, uint32_t minor)
{
    return (minor & 0xffu) | (uint64_t{major & 0xfffu} << 8) | (uint64_t{minor & ~0xffu} << 12);
}

// Mount the boot /dev: a root-owned 0755, nosuid tmpfs (reported as "tmpfs")
// holding the standard character devices (null, zero, full, random, urandom,
// tty, console), /dev/fd and stdio links, and the dri/shm directories that
// later boot steps use. Device nodes are real inodes with st_rdev, so a bind
// of /dev, or of single nodes, into a mount namespace keeps them working.
// Must run after the tmpfs budget is configured and before the canonical
// device anchors are initialised. Non-fatal: returns a negative errno.
int DevtmpfsMountBoot();

} // namespace brook
