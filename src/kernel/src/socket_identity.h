#pragma once
#include <stdint.h>

namespace brook {
struct SocketIdentity {
    uint64_t inode;
    uint32_t uid, gid; // canonical initial-namespace credentials
};

// Separate from proc's high-bit inode domain. Never recycle an identity when
// a socket slot is reused; exhaustion fails closed instead of wrapping.
class SocketInodeAllocator {
    uint64_t next = uint64_t{1} << 62;
public:
    explicit constexpr SocketInodeAllocator(uint64_t first = uint64_t{1} << 62) : next(first) {}
    uint64_t allocate() {
        auto value = __atomic_load_n(&next, __ATOMIC_RELAXED);
        for (;;) {
            if (!value || value >= (uint64_t{1} << 63)) return 0;
            if (__atomic_compare_exchange_n(&next, &value, value + 1, false,
                                             __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return value;
        }
    }
};

inline bool SocketIdentityLabel(uint64_t inode, char* out, uint32_t capacity) {
    char digits[20]; uint32_t count = 0;
    do { digits[count++] = static_cast<char>('0' + inode % 10); inode /= 10; } while (inode);
    if (capacity < 10 + count) { if (capacity) out[0] = 0; return false; }
    const char prefix[] = "socket:[";
    uint32_t at = 0;
    for (; at < 8; ++at) out[at] = prefix[at];
    while (count) out[at++] = digits[--count];
    out[at++] = ']'; out[at] = 0; return true;
}
}
