#pragma once

#include "large_ext2_geometry.h"
#include "ssd_trim_plan.h"

namespace brook {

struct Ext2TrimGroup {
    TrimRange reserved[4]{};
    uint32_t reservedCount = 0;
    uint32_t freeBlocks = 0;
};

constexpr uint32_t Ext2TrimSectorsPerBlock = 8;
constexpr uint32_t Ext2TrimBlocksPerGroup = 32768;

// Only for the admitted classic ext2 profile from LargeExt2ClassifyHeader.
// Validate structural allocations independently of the free block counters.
inline bool Ext2TrimValidateGroup(const LargeExt2Geometry& geometry,
                                  TrimRange partition, uint32_t group,
                                  uint32_t inodesPerGroup,
                                  const uint8_t* descriptor,
                                  const uint8_t* disk, const uint8_t* cached,
                                  uint32_t bitmapBytes, Ext2TrimGroup& out)
{
    out = {};
    if (!disk || !cached || bitmapBytes != 4096 ||
        partition.sectors != uint64_t(geometry.blocks) * Ext2TrimSectorsPerBlock ||
        !TrimRangeInside(partition, {0, TrimLbaLimit}) ||
        !LargeExt2GroupDescriptorValid(geometry, group, inodesPerGroup, descriptor, 32))
        return false;

    const uint64_t begin = uint64_t(group) * Ext2TrimBlocksPerGroup;
    uint64_t first = 0, end = 0;
    if (!LargeExt2GroupBounds(geometry, group, first, end))
        return false;
    Ext2TrimGroup result{};
    auto reserve = [&](uint64_t block, uint64_t count) {
        for (uint64_t i = block - begin; i < block - begin + count; ++i) {
            const uint8_t mask = 1u << (i % 8);
            if (!(disk[i / 8] & cached[i / 8] & mask))
                return false;
        }
        result.reserved[result.reservedCount++] = {
            partition.lba + block * Ext2TrimSectorsPerBlock,
            count * Ext2TrimSectorsPerBlock
        };
        return true;
    };
    if ((first > begin && !reserve(begin, first - begin)) ||
        !reserve(LargeExt2Read32(descriptor), 1) ||
        !reserve(LargeExt2Read32(descriptor + 4), 1) ||
        !reserve(LargeExt2Read32(descriptor + 8), inodesPerGroup / 32))
        return false;

    const uint32_t blocks = uint32_t(end - begin);
    result.freeBlocks = LargeExt2Read16(descriptor + 12);
    if (TrimCountFree(disk, blocks) != result.freeBlocks ||
        TrimCountFree(cached, blocks) != result.freeBlocks)
        return false;
    out = result;
    return true;
}

} // namespace brook
