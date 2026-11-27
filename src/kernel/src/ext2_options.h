#pragma once
// Parsing of ext2 tuning options (QEMU fw_cfg opt/... or the initrd's /etc markers).
// Pure; host-tested in test_ext2_options.
#include <stdint.h>

namespace brook {
// Decimal in [low, high]; anything else (empty, > 5 digits, junk, out of range) -> fallback.
// One trailing newline (a file written with echo) is ignored.
inline uint32_t Ext2ParseBounded(const char* value, uint32_t length,
                                 uint32_t low, uint32_t high, uint32_t fallback) {
    if (length && value[length - 1] == '\n') --length;
    if (!length || length > 5) return fallback;
    uint32_t result = 0;
    for (uint32_t i = 0; i < length; ++i) {
        if (value[i] < '0' || value[i] > '9') return fallback;
        result = result * 10 + static_cast<uint32_t>(value[i] - '0');
    }
    return result >= low && result <= high ? result : fallback;
}
} // namespace brook
