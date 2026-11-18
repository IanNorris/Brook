#pragma once

#include "ahci_read_plan.h"
#include "ata_identify.h"

namespace brook {

struct TrimRange {
    uint64_t lba;
    uint64_t sectors;
};

constexpr uint64_t TrimLbaLimit = 1ull << 48;
constexpr uint32_t TrimPayloadBlockBytes = 512;
constexpr uint32_t TrimEntryBytes = 8;
constexpr uint32_t TrimEntryMaxSectors = 65535;

inline bool TrimRangeInside(TrimRange range, TrimRange bounds)
{
    return bounds.sectors && bounds.lba < TrimLbaLimit &&
           bounds.sectors <= TrimLbaLimit - bounds.lba &&
           range.sectors && range.lba >= bounds.lba &&
           range.lba - bounds.lba < bounds.sectors &&
           range.sectors <= bounds.sectors - (range.lba - bounds.lba);
}

// All ranges use absolute 512-byte device sectors. The caller supplies the
// admitted GPT partition and exclusions derived from verified metadata.
inline bool TrimRangeAllowed(TrimRange range, TrimRange partition,
                             const TrimRange* reserved, uint32_t reservedCount)
{
    if (!TrimRangeInside(range, partition) || (reservedCount && !reserved))
        return false;
    for (uint32_t i = 0; i < reservedCount; ++i) {
        const auto exclusion = reserved[i];
        if (!TrimRangeInside(exclusion, {0, TrimLbaLimit}))
            return false;
        if (range.lba < exclusion.lba + exclusion.sectors &&
            exclusion.lba < range.lba + range.sectors)
            return false;
    }
    return true;
}

inline uint16_t AtaTrimPayloadLimit(const uint8_t* identify, uint32_t bytes)
{
    if (!identify || bytes < 512)
        return 0;
    const uint16_t support = AtaIdentifyWord(identify, 169);
    const uint16_t limit = AtaIdentifyWord(identify, 105);
    return support != 0xffff && (support & 1u) && limit != 0xffff ? limit : 0;
}

inline bool AhciPlanTrim(AhciReadPlan& out, uint16_t payloadBlocks,
                         uint16_t identifyLimit, uint64_t dma, bool dma64)
{
    out = {};
    if (!payloadBlocks || payloadBlocks > identifyLimit ||
        !AhciReadPlanDma(out, dma, uint32_t(payloadBlocks) * TrimPayloadBlockBytes, dma64))
        return false;
    out.commandFlags |= 1u << 6;
    out.fis[2] = 0x06; // Non-queued ATA DATA SET MANAGEMENT.
    out.fis[3] = 0x01; // TRIM feature.
    out.fis[12] = uint8_t(payloadBlocks);
    out.fis[13] = uint8_t(payloadBlocks >> 8);
    return true;
}

// The caller zeroes the full payload before adding entries. A zero count
// marks unused entries, so no device read-after-trim behavior is required.
inline bool AtaEncodeTrimEntry(uint8_t* entry, TrimRange range)
{
    if (!entry || range.sectors > TrimEntryMaxSectors ||
        !TrimRangeInside(range, {0, TrimLbaLimit}))
        return false;
    for (uint32_t i = 0; i < 6; ++i)
        entry[i] = uint8_t(range.lba >> (8 * i));
    entry[6] = uint8_t(range.sectors);
    entry[7] = uint8_t(range.sectors >> 8);
    return true;
}

// Validate the complete immutable plan before handing any payload to the
// caller. The scratch space is reusable DMA storage owned by the caller.
template<class Submit>
bool TrimVisitPayloads(const TrimRange* ranges, uint32_t count,
                       TrimRange partition, const TrimRange* reserved,
                       uint32_t reservedCount, uint8_t* scratch,
                       uint32_t scratchBytes, uint16_t identifyLimit, Submit submit)
{
    uint32_t payloadBlocks = scratchBytes / TrimPayloadBlockBytes;
    if (payloadBlocks > identifyLimit)
        payloadBlocks = identifyLimit;
    if (!scratch || !payloadBlocks || (count && !ranges))
        return false;

    uint64_t previousEnd = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (ranges[i].lba < previousEnd ||
            !TrimRangeAllowed(ranges[i], partition, reserved, reservedCount))
            return false;
        previousEnd = ranges[i].lba + ranges[i].sectors;
    }

    constexpr uint32_t entriesPerBlock = TrimPayloadBlockBytes / TrimEntryBytes;
    const uint32_t capacity = payloadBlocks * entriesPerBlock;
    uint32_t used = 0;
    for (uint32_t i = 0; i < count; ++i) {
        auto range = ranges[i];
        while (range.sectors) {
            const uint64_t sectors = range.sectors > TrimEntryMaxSectors ?
                                     TrimEntryMaxSectors : range.sectors;
            if (!AtaEncodeTrimEntry(scratch + used * TrimEntryBytes, {range.lba, sectors}))
                return false;
            range.lba += sectors;
            range.sectors -= sectors;
            if (++used == capacity) {
                if (!submit(uint16_t(payloadBlocks)))
                    return false;
                used = 0;
            }
        }
    }
    if (!used)
        return true;
    const uint32_t blocks = (used + entriesPerBlock - 1) / entriesPerBlock;
    for (uint32_t i = used * TrimEntryBytes; i < blocks * TrimPayloadBlockBytes; ++i)
        scratch[i] = 0;
    return submit(uint16_t(blocks));
}

inline uint32_t TrimCountFree(const uint8_t* bitmap, uint32_t blocks)
{
    uint32_t free = 0;
    for (uint32_t i = 0; i < blocks; ++i)
        if (!(bitmap[i / 8] & (1u << (i % 8))))
            ++free;
    return free;
}

// Takes validated bitmap snapshots. The caller must keep allocation excluded
// until all submitted ranges retire. The group length omits bitmap padding.
template<class Emit>
bool TrimVisitCommonFreeRuns(const uint8_t* disk, const uint8_t* memory,
                             uint32_t blocks, uint32_t minBlocks, Emit emit)
{
    if (!disk || !memory || !minBlocks)
        return false;
    uint32_t start = 0;
    for (uint32_t i = 0; i < blocks; ++i) {
        if (!((disk[i / 8] | memory[i / 8]) & (1u << (i % 8))))
            continue;
        if (i - start >= minBlocks && !emit(start, i - start))
            return false;
        start = i + 1;
    }
    return blocks - start < minBlocks || emit(start, blocks - start);
}

} // namespace brook
