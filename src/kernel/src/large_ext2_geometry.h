#pragma once
#include <stdint.h>

namespace brook {
// Geometry helpers for a prospective direct-partition ext2 store. These do
// not admit or mount a device; identity, superblock features and clean state
// must be checked separately. The group limit bounds descriptor storage.
struct LargeExt2Geometry {
    uint32_t blocks = 0, groups = 0, descriptorBytes = 0, descriptorBlocks = 0;
};
inline uint16_t LargeExt2Read16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
inline uint32_t LargeExt2Read32(const uint8_t* p) {
    return uint32_t(LargeExt2Read16(p)) | (uint32_t(LargeExt2Read16(p + 2)) << 16);
}

inline bool LargeExt2GeometryMake(uint32_t blocks, LargeExt2Geometry& out) {
    out = {};
    if (!blocks) return false;
    const uint64_t groups = (uint64_t(blocks) + 32767) / 32768;
    if (!groups || groups > 4096) return false;
    const uint64_t bytes = groups * 32;
    out = {blocks, uint32_t(groups), uint32_t(bytes), uint32_t((bytes + 4095) / 4096)};
    return true;
}

enum class LargeExt2HeaderState { Invalid, NeedsCheck, Clean };
// A format and identity gate, not a filesystem consistency check. The device
// selection and partition size must already have been independently verified.
inline LargeExt2HeaderState LargeExt2ClassifyHeader(const uint8_t* sb, uint32_t size,
        uint64_t expectedBytes, const uint8_t* expectedUuid, LargeExt2Geometry& out) {
    out = {};
    if (!sb || size != 1024 || !expectedUuid || !expectedBytes) return LargeExt2HeaderState::Invalid;
    const uint16_t state = LargeExt2Read16(sb + 58);
    if (LargeExt2Read16(sb + 56) != 0xef53 || (state & ~uint16_t{3}) ||
        LargeExt2Read32(sb + 72) != 0 || LargeExt2Read32(sb + 76) != 1 ||
        LargeExt2Read16(sb + 88) != 128 || LargeExt2Read32(sb + 20) != 0 ||
        LargeExt2Read32(sb + 24) != 2 || LargeExt2Read32(sb + 28) != 2 ||
        LargeExt2Read32(sb + 32) != 32768 || LargeExt2Read32(sb + 36) != 32768 ||
        LargeExt2Read32(sb + 92) != 0 || LargeExt2Read32(sb + 96) != 2 ||
        LargeExt2Read32(sb + 100) != 3 || LargeExt2Read32(sb + 84) != 11 ||
        LargeExt2Read16(sb + 90) != 0 || LargeExt2Read16(sb + 206) != 0)
        return LargeExt2HeaderState::Invalid;
    uint8_t nonzero = 0;
    for (unsigned i = 0; i < 16; ++i) {
        nonzero |= expectedUuid[i];
        if (expectedUuid[i] != sb[104 + i]) return LargeExt2HeaderState::Invalid;
    }
    if (!nonzero) return LargeExt2HeaderState::Invalid;
    const uint32_t blocks = LargeExt2Read32(sb + 4), inodes = LargeExt2Read32(sb);
    const uint32_t ipg = LargeExt2Read32(sb + 40);
    LargeExt2Geometry geometry{};
    if (!LargeExt2GeometryMake(blocks, geometry) || uint64_t(blocks) * 4096 != expectedBytes ||
        LargeExt2Read32(sb + 8) != 0 || LargeExt2Read32(sb + 12) > blocks ||
        LargeExt2Read32(sb + 16) > inodes || !ipg || ipg > 32768 || ipg % 32 ||
        inodes < 11 || uint64_t(ipg) * geometry.groups != inodes)
        return LargeExt2HeaderState::Invalid;
    out = geometry;
    return state == 1 ? LargeExt2HeaderState::Clean : LargeExt2HeaderState::NeedsCheck;
}

inline bool LargeExt2HasBackup(uint32_t group) {
    if (group < 2) return true;
    const uint32_t primes[] = {3, 5, 7};
    for (uint32_t prime : primes) {
        uint32_t n = group;
        while (n > 1 && n % prime == 0) n /= prime;
        if (n == 1) return true;
    }
    return false;
}

// Return the first block not reserved for this group's sparse superblock and
// descriptor-table copy. Callers must establish 4 KiB blocks, first data block
// zero, 32768 blocks/group, 32-byte descriptors and classic sparse_super;
// meta_bg, sparse_super2 and reserved GDT blocks must be absent. This does not
// validate bitmap/inode-table placement or non-overlap. Check the return value
// before using either output; failure leaves their contents unspecified.
inline bool LargeExt2GroupBounds(const LargeExt2Geometry& geometry, uint32_t group,
                                 uint64_t& first, uint64_t& end) {
    LargeExt2Geometry checked{};
    if (!LargeExt2GeometryMake(geometry.blocks, checked) ||
        checked.groups != geometry.groups || checked.descriptorBytes != geometry.descriptorBytes ||
        checked.descriptorBlocks != geometry.descriptorBlocks || group >= checked.groups)
        return false;
    const uint64_t begin = uint64_t(group) * 32768;
    end = begin + 32768;
    if (end > geometry.blocks) end = geometry.blocks;
    first = begin + (LargeExt2HasBackup(group) ? 1 + geometry.descriptorBlocks : 0);
    return first < end;
}

// Validate one classic 32-byte group descriptor after separate superblock
// feature/identity checks, including an inode size of exactly 128 bytes.
// Bitmap contents and cross-group ownership are not checked; all three
// metadata structures must be local to this group. Free counters are bounded,
// not reconciled against metadata or bitmap contents.
inline bool LargeExt2GroupDescriptorValid(const LargeExt2Geometry& geometry,
                                           uint32_t group, uint32_t inodesPerGroup,
                                           const uint8_t* descriptor, uint32_t size) {
    if (!descriptor || size != 32 || !inodesPerGroup ||
        inodesPerGroup > 32768 || inodesPerGroup % 32) return false;
    uint64_t first = 0, end = 0;
    if (!LargeExt2GroupBounds(geometry, group, first, end)) return false;
    auto get16 = [](const uint8_t* p) -> uint16_t {
        return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
    };
    auto get32 = [&](const uint8_t* p) -> uint32_t {
        return uint32_t(get16(p)) | (uint32_t(get16(p + 2)) << 16);
    };
    const uint64_t blockMap = get32(descriptor), inodeMap = get32(descriptor + 4);
    const uint64_t table = get32(descriptor + 8);
    const uint64_t tableBlocks = (uint64_t(inodesPerGroup) * 128 + 4095) / 4096;
    if (blockMap < first || blockMap >= end || inodeMap < first || inodeMap >= end ||
        blockMap == inodeMap || table < first || table >= end || tableBlocks > end - table ||
        (blockMap >= table && blockMap < table + tableBlocks) ||
        (inodeMap >= table && inodeMap < table + tableBlocks)) return false;
    const uint64_t begin = uint64_t(group) * 32768;
    return get16(descriptor + 12) <= end - begin &&
           get16(descriptor + 14) <= inodesPerGroup &&
           get16(descriptor + 16) <= inodesPerGroup;
}
}
