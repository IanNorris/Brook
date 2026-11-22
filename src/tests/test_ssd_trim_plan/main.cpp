#include "ext2_trim_plan.h"
#include "ahci_direct_layout.h"

#include <cassert>
#include <cstring>
#include <atomic>
#include <thread>

using namespace brook;

static void TestExt2Metadata()
{
    // Group 3 ends early and contains sparse-super backup metadata.
    LargeExt2Geometry geometry{};
    assert(LargeExt2GeometryMake(3 * 32768 + 16, geometry));
    const TrimRange partition{2048, uint64_t(geometry.blocks) * 8};
    const uint32_t base = 3 * 32768;
    uint8_t descriptor[32]{};
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t block = base + 2 + i;
        for (uint32_t byte = 0; byte < 4; ++byte)
            descriptor[4 * i + byte] = uint8_t(block >> (8 * byte));
    }
    descriptor[12] = 11;
    uint8_t disk[4096]{0x1f};
    uint8_t cache[4096]{0x1f};
    Ext2TrimGroup plan{};
    assert(Ext2TrimValidateGroup(geometry, partition, 3, 32, descriptor,
                                 disk, cache, sizeof(disk), plan));
    assert(plan.freeBlocks == 11 && plan.reservedCount == 4);
    const uint64_t first = partition.lba + uint64_t(base) * 8;
    assert(!TrimRangeAllowed({first + 8, 1}, partition, plan.reserved, plan.reservedCount));
    assert(!TrimRangeAllowed({first + 32, 1}, partition, plan.reserved, plan.reservedCount));
    assert(TrimRangeAllowed({first + 40, 88}, partition, plan.reserved, plan.reservedCount));
    cache[0] = 0x3d; // Same count, but claims a backup descriptor block is free.
    assert(!Ext2TrimValidateGroup(geometry, partition, 3, 32, descriptor,
                                  disk, cache, sizeof(disk), plan));
    cache[0] = 0x3f; // Metadata still reserved, free count disagrees.
    assert(!Ext2TrimValidateGroup(geometry, partition, 3, 32, descriptor,
                                  disk, cache, sizeof(disk), plan));
}

static void TestCleanScan()
{
    constexpr uint32_t totalBlocks = 32768 + 8;
    const TrimRange partition{2048, uint64_t(totalBlocks) * 8};
    const uint8_t uuid[16]{1};
    uint8_t super[1024]{};
    auto put = [](uint8_t* p, uint32_t value, unsigned bytes = 4) {
        for (unsigned i = 0; i < bytes; ++i)
            p[i] = uint8_t(value >> (8 * i));
    };
    put(super, 64);
    put(super + 4, totalBlocks);
    put(super + 12, 32766);
    put(super + 24, 2);
    put(super + 28, 2);
    put(super + 32, 32768);
    put(super + 36, 32768);
    put(super + 40, 32);
    put(super + 56, 0xef53, 2);
    put(super + 58, 1, 2);
    put(super + 76, 1);
    put(super + 84, 11);
    put(super + 88, 128, 2);
    put(super + 96, 2);
    put(super + 100, 3);
    memcpy(super + 104, uuid, sizeof(uuid));
    uint8_t desc[2][32]{};
    uint8_t bitmaps[2][4096]{{0x1f}, {0x1f}};
    for (unsigned g = 0; g < 2; ++g) {
        for (unsigned field = 0; field < 3; ++field)
            put(desc[g] + 4 * field, g * 32768 + 2 + field);
        put(desc[g] + 12, (g ? 8 : 32768) - 5, 2);
    }
    auto read = [&](uint64_t offset, void* out, uint32_t bytes) {
        if (offset == 1024 && bytes == sizeof(super)) {
            memcpy(out, super, bytes);
            return true;
        }
        for (unsigned g = 0; g < 2; ++g) {
            if (offset == 4096 + g * 32 && bytes == sizeof(desc[g])) {
                memcpy(out, desc[g], bytes);
                return true;
            }
            if (offset == uint64_t(g * 32768 + 2) * 4096 && bytes == sizeof(bitmaps[g])) {
                memcpy(out, bitmaps[g], bytes);
                return true;
            }
        }
        return false;
    };
    Ext2TrimScan scratch{};
    unsigned ranges = 0;
    auto emit = [&](TrimRange range) {
        assert(ranges < 2);
        assert(range.lba == partition.lba + (ranges * 32768 + 5) * 8);
        assert(range.sectors == (ranges ? 3 : 32763) * 8);
        ++ranges;
        return true;
    };
    auto scan = [&] { return Ext2TrimScanClean(partition, uuid, 1, scratch, read, emit); };
    assert(scan() == Ext2TrimScanResult::Ok && ranges == 2);
    // Use the boot path's lease layout. A competing raw writer (including
    // the would-be desktop adapter before ownership transfer) may not
    // allocate any block in the partition while the emitter is outstanding.
    AhciDirectLayout layout{};
    layout.esp.first = 34;
    layout.esp.count = partition.lba - 34;
    layout.data.first = partition.lba;
    layout.data.count = partition.sectors;
    layout.frontBytes = 34 * 512;
    layout.backStart = (partition.lba + partition.sectors) * 512;
    layout.backBytes = 33 * 512;
    constexpr uintptr_t disk = 1, dataOwner = 2, espOwner = 3, desktopAdapter = 4;
    BlockWriteLease reservations[4]{};
    assert(AhciDirectGuardLeases(layout, disk, dataOwner, espOwner, reservations));
    BlockWriteLeases leases;
    assert(leases.Install(reservations, 4, true));
    std::atomic<bool> emitting{false}, attempted{false};
    std::thread writer([&] {
        while (!emitting.load(std::memory_order_acquire))
            std::this_thread::yield();
        const uint64_t offset = (partition.lba + 5 * 8) * 512;
        assert(!leases.AllowOrdinary(disk, offset, 4096));
        assert(!leases.AllowOwner(disk, desktopAdapter, offset, 4096));
        assert(leases.AllowOwner(disk, dataOwner, partition.lba * 512 + 1082, 2));
        attempted.store(true, std::memory_order_release);
    });
    ranges = 0;
    assert(Ext2TrimScanClean(partition, uuid, 1, scratch, read, [&](TrimRange range) {
        emitting.store(true, std::memory_order_release);
        while (!attempted.load(std::memory_order_acquire))
            std::this_thread::yield();
        return emit(range);
    }) == Ext2TrimScanResult::Ok && ranges == 2);
    writer.join();
    ranges = 0;
    bitmaps[1][0] |= 0x20;
    assert(scan() == Ext2TrimScanResult::Invalid && ranges == 0);
    bitmaps[1][0] = 0x1f;
    put(super + 12, 32765);
    assert(scan() == Ext2TrimScanResult::Invalid && ranges == 0);
    put(super + 58, 0, 2);
    assert(scan() == Ext2TrimScanResult::Dirty && ranges == 0);
}

int main()
{
    SsdTrimRequest request{};
    assert(SsdTrimParseRequest(nullptr, 0, request));
    assert(!request.run && request.minimumMiB == 1 && !request.maximumMiB);
    assert(SsdTrimParseRequest("run 1 16\n", 9, request));
    assert(request.run && request.minimumMiB == 1 && request.maximumMiB == 16);
    assert(!SsdTrimParseRequest("run 2 1\n", 8, request));
    assert(!SsdTrimParseRequest("run 0\n", 6, request));
    assert(!SsdTrimParseRequest("run", 3, request));
    TestExt2Metadata();
    TestCleanScan();
    const TrimRange partition{2048, 524288};
    const TrimRange reserved[] = {{2048, 8}, {3000, 16}, {7000, 128}};
    assert(TrimRangeAllowed({2056, 944}, partition, reserved, 3));
    assert(!TrimRangeAllowed({2055, 1}, partition, reserved, 3));
    assert(!TrimRangeAllowed({2999, 2}, partition, reserved, 3));
    assert(!TrimRangeAllowed({7001, 1}, partition, reserved, 3));
    assert(!TrimRangeAllowed({2047, 1}, partition, reserved, 3));
    assert(!TrimRangeAllowed({526335, 2}, partition, reserved, 3));
    assert(!TrimRangeAllowed({UINT64_MAX, 2}, partition, reserved, 3));

    uint8_t identify[512]{};
    identify[210] = 2;
    assert(AtaTrimPayloadLimit(identify, sizeof(identify)) == 0);
    identify[338] = 1;
    identify[210] = 0;
    assert(AtaTrimPayloadLimit(identify, sizeof(identify)) == 1);
    identify[210] = 2;
    assert(AtaTrimPayloadLimit(identify, sizeof(identify)) == 2);
    assert(AtaTrimPayloadLimit(identify, 511) == 0);
    AhciReadPlan plan{};
    assert(!AhciPlanTrim(plan, 3, 2, 0x1000, false));
    assert(!AhciPlanTrim(plan, 2, 2, 0xfffffe00, false));
    assert(AhciPlanTrim(plan, 2, 2, 0x1000, false));
    assert(plan.fis[2] == 0x06 && plan.fis[3] == 1 && plan.fis[12] == 2);
    assert(plan.fis[13] == 0 && plan.commandFlags == 0x45);
    assert(plan.prdtEntries == 1 && plan.byteCountMinusOne == 1023);
    uint8_t entry[8]{};
    assert(AtaEncodeTrimEntry(entry, {0x123456789abull, 0xcdef}));
    const uint8_t expected[] = {0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0xef, 0xcd};
    for (unsigned i = 0; i < 8; ++i)
        assert(entry[i] == expected[i]);
    assert(!AtaEncodeTrimEntry(entry, {0, 65536}));
    assert(!AtaEncodeTrimEntry(entry, {TrimLbaLimit - 1, 2}));

    uint8_t scratch[1024]{};
    const TrimRange large[] = {{2048, uint64_t(65535) * 64 + 1}};
    const TrimRange largePartition{2048, 5000000};
    unsigned commands = 0;
    assert(TrimVisitPayloads(large, 1, largePartition, nullptr, 0, scratch,
                             sizeof(scratch), 1, [&](uint16_t blocks) {
        assert(blocks == 1);
        if (++commands == 1) {
            assert(scratch[6] == 0xff && scratch[511] == 0xff);
        } else {
            uint8_t last[8]{};
            assert(AtaEncodeTrimEntry(last, {2048 + uint64_t(65535) * 64, 1}));
            for (unsigned i = 0; i < 8; ++i)
                assert(last[i] == scratch[i]);
            for (unsigned i = 8; i < 512; ++i)
                assert(scratch[i] == 0);
        }
        return true;
    }));
    assert(commands == 2);
    const TrimRange badPlan[] = {large[0], {5000000, 2049}};
    assert(!TrimVisitPayloads(badPlan, 2, largePartition, nullptr, 0, scratch,
                              sizeof(scratch), 1, [&](uint16_t) {
        ++commands;
        return true;
    }));
    assert(commands == 2);

    const uint8_t disk[] = {1u << 2, 0};
    const uint8_t memory[] = {1u << 5, 0xff};
    assert(TrimCountFree(memory, 10) == 7);
    const uint32_t starts[] = {0, 3, 6};
    unsigned calls = 0;
    assert(TrimVisitCommonFreeRuns(disk, memory, 10, 2, [&](uint32_t start, uint32_t count) {
        assert(calls < 3 && start == starts[calls++] && count == 2);
        return true;
    }));
    assert(calls == 3);
    assert(!TrimVisitCommonFreeRuns(disk, memory, 10, 2, [](uint32_t, uint32_t) {
        return false;
    }));
}
