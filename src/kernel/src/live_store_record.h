#pragma once
#include <stdint.h>

// /etc/live-store (signed initrd): one line, either
//   guid=<32 hex, GPT on-disk byte order> bytes=<decimal> sha256=<64 hex>
// naming a raw store partition on the boot stick, or
//   lba=<decimal> sectors=<decimal> bytes=<decimal> sha256=<64 hex>
// naming a raw extent (a store slot written over the network) on the boot
// stick's disk; plus the exact image length (a multiple of 4096) and its
// SHA-256. Pure parser, host-tested.
namespace brook {
struct LiveStoreRecord {
    uint8_t guid[16]{}; bool extent = false; uint64_t firstLba = 0, sectors = 0;
    uint64_t bytes = 0; uint8_t sha256[32]{};
};

inline bool LiveStoreHex(const char*& p, const char* end, uint8_t* out, unsigned count)
{
    auto nibble = [](char c) -> int {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    if (end - p < long(count * 2)) return false;
    for (unsigned i = 0; i < count; ++i) {
        const int hi = nibble(p[0]), lo = nibble(p[1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = uint8_t(hi << 4 | lo); p += 2;
    }
    return true;
}
inline bool LiveStoreKey(const char*& p, const char* end, const char* key)
{
    while (*key) { if (p == end || *p != *key) return false; ++p; ++key; }
    return true;
}
inline bool LiveStoreNumber(const char*& p, const char* end, uint64_t& out)
{
    unsigned digits = 0; out = 0;
    while (p != end && *p >= '0' && *p <= '9') {
        if (++digits > 15) return false;
        out = out * 10 + uint64_t(*p++ - '0');
    }
    return digits != 0;
}
inline bool LiveStoreParse(const char* text, uint32_t length, LiveStoreRecord& out)
{
    out = {};
    const char* p = text; const char* end = text + length;
    if (LiveStoreKey(p, end, "lba=")) {
        out.extent = true;
        if (!LiveStoreNumber(p, end, out.firstLba) || !LiveStoreKey(p, end, " sectors=") ||
            !LiveStoreNumber(p, end, out.sectors)) return false;
    } else if (!LiveStoreKey(p, end, "guid=") || !LiveStoreHex(p, end, out.guid, 16)) return false;
    if (!LiveStoreKey(p, end, " bytes=") || !LiveStoreNumber(p, end, out.bytes)) return false;
    if (!LiveStoreKey(p, end, " sha256=") || !LiveStoreHex(p, end, out.sha256, 32)) return false;
    if (p != end && *p == '\n') ++p;
    if (p != end) return false;
    if (!out.bytes || out.bytes % 4096) return false;
    if (out.extent) return out.firstLba >= 2048 && out.sectors && out.bytes <= out.sectors * 512;
    bool zero = true; for (uint8_t b : out.guid) zero &= b == 0;
    return !zero;
}
}
