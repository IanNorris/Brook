#pragma once
#include <stdint.h>
namespace brook {
// Existing mappings use slots0..3. Reserve previously unused slot4 for WC;
// reject unfamiliar firmware layouts instead of reinterpreting live mappings.
inline bool CpuPatWithWriteCombining(uint64_t current, uint64_t& target) {
    constexpr uint64_t slot4 = uint64_t{0xff} << 32;
    constexpr uint64_t reset = 0x0007040600070406ULL;
    const uint64_t type = (current >> 32) & 0xff;
    if ((current & ~slot4) != (reset & ~slot4) || (type != 6 && type != 1)) return false;
    target = (current & ~slot4) | (uint64_t{1} << 32);
    return true;
}
} // namespace brook
