#pragma once
#include <stdint.h>

namespace brook {
// Linux extended inode timestamps use a signed 32-bit base plus two epoch
// bits, and retain nanoseconds in the remaining 30 bits of the extra field.
// These pure helpers do not establish that an inode has an extra field, nor
// perform any filesystem I/O or alter current ext2 timestamp behavior.
struct Ext2ExtendedTime { int64_t seconds = 0; uint32_t nanoseconds = 0; };

inline bool Ext2ExtendedTimeDecode(uint32_t base, uint32_t extra, Ext2ExtendedTime& out) {
    const uint32_t nanos = extra >> 2;
    if (nanos >= 1000000000u) return false;
    const int64_t signedBase = base & 0x80000000u ? int64_t(base) - 0x100000000LL : base;
    out = {signedBase + (int64_t(extra & 3u) << 32), nanos};
    return true;
}

inline bool Ext2ExtendedTimeEncode(const Ext2ExtendedTime& value, uint32_t& base, uint32_t& extra) {
    if (value.seconds < -2147483648LL || value.seconds > 15032385535LL ||
        value.nanoseconds >= 1000000000u) return false;
    const uint32_t low = static_cast<uint32_t>(value.seconds);
    const int64_t signedBase = low & 0x80000000u ? int64_t(low) - 0x100000000LL : low;
    const uint64_t epoch = uint64_t(value.seconds - signedBase) >> 32;
    if (epoch > 3) return false;
    base = low;
    extra = (value.nanoseconds << 2) | uint32_t(epoch);
    return true;
}
}
