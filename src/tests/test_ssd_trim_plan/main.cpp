#include "ssd_trim_plan.h"

#include <cassert>

using namespace brook;

int main()
{
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
