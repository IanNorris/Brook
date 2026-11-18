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

struct Ext2TrimScan {
    uint8_t superblock[1024];
    uint8_t descriptor[32];
    uint8_t bitmap[4096];
};

enum class Ext2TrimScanResult { Ok, IoError, Invalid, Dirty, Cancelled };

// Reads partition-relative bytes. The caller must pin the clean filesystem
// and its write lease across both passes. With no mounted allocator, the disk
// bitmap is authoritative. An emitter failure stops before the next group.
template<class Read, class Emit>
Ext2TrimScanResult Ext2TrimScanClean(TrimRange partition, const uint8_t* uuid,
                                   uint32_t minBlocks, Ext2TrimScan& scratch,
                                   Read read, Emit emit)
{
    if (!minBlocks || !TrimRangeInside(partition, {0, TrimLbaLimit}))
        return Ext2TrimScanResult::Invalid;
    constexpr uint32_t blockBytes = 4096;
    if (!read(1024, scratch.superblock, sizeof(scratch.superblock)))
        return Ext2TrimScanResult::IoError;

    LargeExt2Geometry geometry{};
    switch (LargeExt2ClassifyHeader(scratch.superblock, sizeof(scratch.superblock),
                                    partition.sectors * 512, uuid, geometry)) {
    case LargeExt2HeaderState::Invalid:
        return Ext2TrimScanResult::Invalid;
    case LargeExt2HeaderState::NeedsCheck:
        return Ext2TrimScanResult::Dirty;
    case LargeExt2HeaderState::Clean:
        break;
    }
    const uint32_t ipg = LargeExt2Read32(scratch.superblock + 40);
    const uint32_t expectedFree = LargeExt2Read32(scratch.superblock + 12);

    for (unsigned pass = 0; pass < 2; ++pass) {
        uint64_t freeBlocks = 0;
        for (uint32_t group = 0; group < geometry.groups; ++group) {
            if (!read(blockBytes + uint64_t(group) * sizeof(scratch.descriptor),
                      scratch.descriptor, sizeof(scratch.descriptor)))
                return Ext2TrimScanResult::IoError;
            if (!LargeExt2GroupDescriptorValid(geometry, group, ipg,
                                               scratch.descriptor, sizeof(scratch.descriptor)))
                return Ext2TrimScanResult::Invalid;
            const uint64_t bitmap = LargeExt2Read32(scratch.descriptor);
            if (!read(bitmap * blockBytes, scratch.bitmap, sizeof(scratch.bitmap)))
                return Ext2TrimScanResult::IoError;
            Ext2TrimGroup plan{};
            if (!Ext2TrimValidateGroup(geometry, partition, group, ipg, scratch.descriptor,
                                       scratch.bitmap, scratch.bitmap, sizeof(scratch.bitmap), plan))
                return Ext2TrimScanResult::Invalid;
            freeBlocks += plan.freeBlocks;
            if (pass == 0)
                continue;
            const uint64_t groupStart = uint64_t(group) * Ext2TrimBlocksPerGroup;
            const uint64_t remaining = geometry.blocks - groupStart;
            const uint32_t blocks = remaining < Ext2TrimBlocksPerGroup ?
                                    uint32_t(remaining) : Ext2TrimBlocksPerGroup;
            bool valid = true;
            const bool visited = TrimVisitCommonFreeRuns(scratch.bitmap, scratch.bitmap, blocks,
                    minBlocks, [&](uint32_t start, uint32_t count) {
                const TrimRange range{partition.lba + (groupStart + start) * Ext2TrimSectorsPerBlock,
                                      uint64_t(count) * Ext2TrimSectorsPerBlock};
                valid = TrimRangeAllowed(range, partition, plan.reserved, plan.reservedCount);
                return valid && emit(range);
            });
            if (!visited)
                return valid ? Ext2TrimScanResult::Cancelled : Ext2TrimScanResult::Invalid;
        }
        if (freeBlocks != expectedFree)
            return Ext2TrimScanResult::Invalid;
    }
    return Ext2TrimScanResult::Ok;
}

} // namespace brook
