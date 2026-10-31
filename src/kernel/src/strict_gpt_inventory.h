#pragma once

#include <stdint.h>

// Read-only GPT validation for installation inventory. The caller supplies
// already-read sectors; this code never issues I/O or modifies the disk.
namespace brook {

enum class StrictGptResult : uint8_t {
    Ok, UnsupportedGeometry, InvalidHeader, HeaderChecksum, InvalidLayout,
    EntryChecksum, CopiesDiffer, InvalidPartition, OverlappingPartitions
};
enum class StrictMbrResult : uint8_t { Protective, Hybrid, Missing, Invalid };

struct StrictGptHeader {
    uint64_t current, alternate, firstUsable, lastUsable, entriesLba;
    uint32_t count, entrySize, entriesCrc;
    uint8_t diskGuid[16];
};

inline uint32_t StrictLe32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint64_t StrictLe64(const uint8_t* p) {
    return uint64_t(StrictLe32(p)) | (uint64_t(StrictLe32(p + 4)) << 32);
}
inline uint32_t StrictCrc32(const uint8_t* p, uint32_t n, bool header = false) {
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t byte = header && i >= 16 && i < 20 ? 0 : p[i];
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
inline bool StrictAllZero(const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (p[i]) return false;
    return true;
}

// Classify rather than silently treating hybrid MBRs as ordinary GPT. The
// latter require a separate installation decision even if GPT copies agree.
inline StrictMbrResult StrictGptProtectiveMbr(const uint8_t* mbr, uint64_t sectors) {
    if (!mbr || sectors < 2 || mbr[510] != 0x55 || mbr[511] != 0xaa) return StrictMbrResult::Invalid;
    unsigned protective=0, other=0;
    const uint64_t maxLength=sectors-1 > 0xffffffffull ? 0xffffffffull : sectors-1;
    for (unsigned i=0;i<4;++i) {
        const uint8_t* e=mbr+446+i*16;
        if (!e[4]) {if (!StrictAllZero(e,16)) return StrictMbrResult::Invalid;continue;}
        if (e[4]!=0xee) {++other;continue;}
        ++protective;
        if(e[0] || StrictLe32(e+8)!=1 || StrictLe32(e+12)!=maxLength) return StrictMbrResult::Invalid;
    }
    if (!protective) return StrictMbrResult::Missing;
    if (protective!=1) return StrictMbrResult::Invalid;
    return other ? StrictMbrResult::Hybrid : StrictMbrResult::Protective;
}

// A bounded reader can call this before allocating or reading an entry array.
// 512-byte logical sectors and up to 128 entries / 1 MiB are intentional limits.
inline StrictGptResult StrictGptCheckHeader(const uint8_t* h, uint32_t sectorSize,
                                           uint64_t sectors, bool backup,
                                           StrictGptHeader& out) {
    if (!h || sectorSize != 512 || sectors < 4) return StrictGptResult::UnsupportedGeometry;
    static constexpr uint8_t signature[8] = {'E','F','I',' ','P','A','R','T'};
    for (unsigned i = 0; i < 8; ++i) if (h[i] != signature[i]) return StrictGptResult::InvalidHeader;
    const uint32_t size = StrictLe32(h + 12);
    if (StrictLe32(h + 8) != 0x00010000u || size < 92 || size > 512 || StrictLe32(h + 20))
        return StrictGptResult::InvalidHeader;
    if (!StrictAllZero(h + 92, 420)) return StrictGptResult::InvalidHeader;
    if (StrictCrc32(h, size, true) != StrictLe32(h + 16)) return StrictGptResult::HeaderChecksum;
    out.current = StrictLe64(h + 24); out.alternate = StrictLe64(h + 32);
    out.firstUsable = StrictLe64(h + 40); out.lastUsable = StrictLe64(h + 48);
    for (unsigned i = 0; i < 16; ++i) out.diskGuid[i] = h[56 + i];
    out.entriesLba = StrictLe64(h + 72); out.count = StrictLe32(h + 80);
    out.entrySize = StrictLe32(h + 84); out.entriesCrc = StrictLe32(h + 88);
    if (out.current != (backup ? sectors - 1 : 1) || out.alternate != (backup ? 1 : sectors - 1) ||
        StrictAllZero(out.diskGuid, 16) || sectors < 68 || out.firstUsable < 34 || out.firstUsable > out.lastUsable ||
        out.lastUsable >= sectors - 33 || !out.count || out.count > 128 ||
        out.entrySize < 128 || (out.entrySize & (out.entrySize - 1)) || out.entrySize > 8192 ||
        uint64_t(out.count) * out.entrySize > 1024 * 1024)
        return StrictGptResult::InvalidLayout;
    const uint64_t blocks = (uint64_t(out.count) * out.entrySize + 511) / 512;
    if (out.entriesLba >= sectors || blocks > sectors - out.entriesLba)
        return StrictGptResult::InvalidLayout;
    const uint64_t end = out.entriesLba + blocks;
    if ((!backup && (out.entriesLba < 2 || end > out.firstUsable)) ||
        (backup && (out.entriesLba <= out.lastUsable || end > sectors - 1)))
        return StrictGptResult::InvalidLayout;
    return StrictGptResult::Ok;
}

// Both headers must first have passed StrictGptCheckHeader, and each buffer
// must contain the complete, declared entry array.
inline StrictGptResult StrictGptCheckCopies(const StrictGptHeader& a, const uint8_t* ae,
                                           const StrictGptHeader& b, const uint8_t* be,
                                           uint32_t bytes) {
    if (!ae || !be || uint64_t(a.count) * a.entrySize != bytes ||
        a.count != b.count || a.entrySize != b.entrySize || a.entriesCrc != b.entriesCrc ||
        a.firstUsable != b.firstUsable || a.lastUsable != b.lastUsable)
        return StrictGptResult::CopiesDiffer;
    for (unsigned i = 0; i < 16; ++i) if (a.diskGuid[i] != b.diskGuid[i]) return StrictGptResult::CopiesDiffer;
    if (StrictCrc32(ae, bytes) != a.entriesCrc || StrictCrc32(be, bytes) != b.entriesCrc)
        return StrictGptResult::EntryChecksum;
    for (uint32_t i = 0; i < bytes; ++i) if (ae[i] != be[i]) return StrictGptResult::CopiesDiffer;
    for (uint32_t i = 0; i < a.count; ++i) {
        const uint8_t* entry = ae + uint64_t(i) * a.entrySize;
        if (StrictAllZero(entry, 16)) continue;
        if (!StrictAllZero(entry + 128, a.entrySize - 128)) return StrictGptResult::InvalidPartition;
        const uint64_t first = StrictLe64(entry + 32), last = StrictLe64(entry + 40);
        if (StrictAllZero(entry + 16, 16) || first < a.firstUsable || first > last || last > a.lastUsable)
            return StrictGptResult::InvalidPartition;
        for (uint32_t j = 0; j < i; ++j) {
            const uint8_t* prior = ae + uint64_t(j) * a.entrySize;
            if (StrictAllZero(prior, 16)) continue;
            if (!(last < StrictLe64(prior + 32) || first > StrictLe64(prior + 40)))
                return StrictGptResult::OverlappingPartitions;
            bool same = true; for (unsigned k = 0; k < 16; ++k) if (entry[16 + k] != prior[16 + k]) same = false;
            if (same) return StrictGptResult::InvalidPartition;
        }
    }
    return StrictGptResult::Ok;
}

} // namespace brook
