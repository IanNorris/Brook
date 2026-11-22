#include "ssd_trim_log.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace brook;

static constexpr uint64_t PartitionLba = 526336;
static constexpr uint64_t Group = SsdTrimLogSectorsPerGroup;

static void TestGroupSummaryCap()
{
    SsdTrimGroupAccumulator groups(PartitionLba, 2);
    std::vector<SsdTrimGroupSummary> lines;
    auto emit = [&](const SsdTrimGroupSummary& g) { lines.push_back(g); };
    groups.Add({PartitionLba + 2048, 2048}, emit);
    groups.Add({PartitionLba + 8192, 4096}, emit);
    assert(lines.empty()); // the current group is still open
    groups.Add({PartitionLba + Group + 16, 256}, emit);
    assert(lines.size() == 1 && lines[0].group == 0 && lines[0].ranges == 2 && lines[0].sectors == 6144);
    groups.Add({PartitionLba + 3 * Group, 512}, emit);
    groups.Add({PartitionLba + 3 * Group + 1024, 512}, emit);
    groups.Add({PartitionLba + 7 * Group, 8}, emit);
    groups.Finish(emit);
    assert(lines.size() == 2 && lines[1].group == 1 && lines[1].ranges == 1 && lines[1].sectors == 256);
    assert(groups.groups() == 4 && groups.printed() == 2 && groups.suppressedGroups() == 2);
    assert(groups.suppressedRanges() == 3 && groups.suppressedSectors() == 1032);

    SsdTrimGroupAccumulator empty(PartitionLba);
    empty.Finish([](const SsdTrimGroupSummary&) { assert(false); });
    assert(!empty.groups() && !empty.printed());
}

static void TestGroupOf()
{
    assert(SsdTrimGroupOf(PartitionLba, PartitionLba) == 0);
    assert(SsdTrimGroupOf(PartitionLba, PartitionLba + Group - 8) == 0);
    assert(SsdTrimGroupOf(PartitionLba, PartitionLba + Group) == 1);
    // The Dell sample range: fs block 8213103 lies in group 250.
    assert(SsdTrimGroupOf(PartitionLba, 66231160) == 250);
    assert(SsdTrimGroupOf(PartitionLba, 100) == 0);
}

static void TestCapacityCountsDrops()
{
    SsdTrimLogRange ranges[2];
    SsdTrimLogCommand commands[1];
    SsdTrimLog log{ranges, 2, 0, commands, 1, 0};
    log.AddRange({10, 8});
    log.AddRange({20, 8});
    log.AddRange({30, 8});
    log.AddCommand(1, 5, true, true);
    log.AddCommand(1, 6, true, true);
    assert(log.rangeCount == 2 && log.rangesDropped == 1);
    assert(log.commandCount == 1 && log.commandsDropped == 1);

    SsdTrimLog none{};
    none.AddRange({1, 1});
    none.AddCommand(1, 1, false, true);
    assert(!none.rangeCount && none.rangesDropped == 1 && none.commandsDropped == 1);
}

static std::vector<std::string> Render(const SsdTrimLog& log, const SsdTrimLogHeader& header,
                                       const SsdTrimLogResult& result)
{
    std::vector<std::string> lines;
    char line[SsdTrimLogLineMax];
    for (uint32_t i = 0; i < SsdTrimLogLineCount(log); ++i) {
        assert(SsdTrimLogLine(log, header, result, i, line, sizeof(line)));
        lines.emplace_back(line);
    }
    assert(!SsdTrimLogLine(log, header, result, uint32_t(SsdTrimLogLineCount(log)), line, sizeof(line)));
    return lines;
}

// Batches tag ranges and the commands that carried them; the commands come
// from the same payload visitor the AHCI path uses.
static void TestBatchesAndRendering()
{
    SsdTrimLogRange ranges[64];
    SsdTrimLogCommand commands[16];
    SsdTrimLog log{ranges, 64, 0, commands, 16, 0};
    const TrimRange partition{PartitionLba, 64 * Group};
    // Batch 0: 70 entries of at most 65535 sectors -> two 512-byte payloads.
    std::vector<TrimRange> first;
    for (uint32_t i = 0; i < 14; ++i)
        first.push_back({PartitionLba + 2 * Group * i, 5 * 65535});
    for (const auto& r : first)
        log.AddRange(r);
    uint8_t scratch[512];
    uint64_t ns = 100;
    assert(TrimVisitPayloads(first.data(), uint32_t(first.size()), partition, nullptr, 0,
                             scratch, sizeof(scratch), 1, [&](uint16_t blocks) {
        log.AddCommand(blocks, ns++, true, true);
        return true;
    }));
    log.EndBatch();
    assert(log.commandCount == 2 && commands[0].batch == 0 && commands[1].batch == 0);
    // Batch 1: one range, command fails.
    log.AddRange({PartitionLba + 20 * Group, 32768});
    log.AddCommand(1, 30000000000ull, true, false);
    log.EndBatch();

    const SsdTrimLogHeader header{true, 1, 16, PartitionLba, 499591816, 1795370000123456789ull};
    const SsdTrimLogResult result{15, 3, 0, 512ull * (14 * 5 * 65535 + 32768), 30000000201ull,
                                  30000000000ull, 31000000000ull, -5};
    const auto lines = Render(log, header, result);
    assert(lines.size() == 2 + 15 + 3);
    assert(lines[0] == "{\"type\":\"header\",\"version\":1,\"mode\":\"run\",\"min_mib\":1,\"max_mib\":16,"
                       "\"partition_lba\":526336,\"partition_sectors\":499591816,"
                       "\"started_unix_ns\":1795370000123456789}\n");
    assert(lines[1] == "{\"type\":\"range\",\"i\":0,\"lba\":526336,\"sectors\":327675,\"group\":0,\"batch\":0}\n");
    assert(lines[15] == "{\"type\":\"range\",\"i\":14,\"lba\":5769216,\"sectors\":32768,\"group\":20,\"batch\":1}\n");
    assert(lines[16] == "{\"type\":\"command\",\"i\":0,\"batch\":0,\"payload_blocks\":1,\"ns\":100,"
                        "\"submitted\":true,\"ok\":true}\n");
    assert(lines[18] == "{\"type\":\"command\",\"i\":2,\"batch\":1,\"payload_blocks\":1,\"ns\":30000000000,"
                        "\"submitted\":true,\"ok\":false}\n");
    char expected[512];
    std::snprintf(expected, sizeof(expected),
                  "{\"type\":\"summary\",\"mode\":\"run\",\"ranges\":15,\"bytes\":%llu,\"dsm_commands\":3,"
                  "\"planned_commands\":0,\"command_total_ns\":30000000201,\"command_max_ns\":30000000000,"
                  "\"duration_ns\":31000000000,\"error\":-5,\"ranges_logged\":15,\"ranges_dropped\":0,"
                  "\"commands_logged\":3,\"commands_dropped\":0}\n",
                  (unsigned long long)result.bytes);
    assert(lines[19] == expected);
}

static void TestDryRunAndOverflow()
{
    SsdTrimLog log{};
    SsdTrimLogHeader header{};
    header.minimumMiB = 1;
    SsdTrimLogResult result{};
    result.error = -2147483647 - 1;
    const auto lines = Render(log, header, result);
    assert(lines.size() == 2);
    assert(lines[0].find("\"mode\":\"dry-run\"") != std::string::npos);
    assert(lines[1].find("\"error\":-2147483648,") != std::string::npos);

    char small[24];
    assert(!SsdTrimLogLine(log, header, result, 0, small, sizeof(small)));
    assert(std::strlen(small) < sizeof(small)); // bounded, still terminated
    assert(!SsdTrimLogLine(log, header, result, 0, nullptr, 0));
}

// The largest rendered line must fit the kernel's line buffer.
static void TestWorstCaseLineFits()
{
    SsdTrimLogRange ranges[1]{{~0ull, ~0ull, ~0u}};
    SsdTrimLogCommand commands[1]{{~0ull, ~0u, 0xffff, true, false}};
    SsdTrimLog log{ranges, 1, 1, commands, 1, 1};
    log.rangesDropped = log.commandsDropped = ~0ull;
    const SsdTrimLogHeader header{false, ~0u, ~0u, 0, ~0ull, ~0ull};
    const SsdTrimLogResult result{~0u, ~0u, ~0u, ~0ull, ~0ull, ~0ull, ~0ull, -2147483647 - 1};
    char line[SsdTrimLogLineMax];
    size_t longest = 0;
    for (uint32_t i = 0; i < SsdTrimLogLineCount(log); ++i) {
        assert(SsdTrimLogLine(log, header, result, i, line, sizeof(line)));
        longest = std::max(longest, std::strlen(line));
    }
    std::printf("longest worst-case line: %zu bytes\n", longest);
}

int main()
{
    TestGroupSummaryCap();
    TestGroupOf();
    TestCapacityCountsDrops();
    TestBatchesAndRendering();
    TestDryRunAndOverflow();
    TestWorstCaseLineFits();
    std::puts("test_ssd_trim_log: PASS");
    return 0;
}
