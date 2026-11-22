#pragma once

// Records one maintenance-boot TRIM pass for later persistence. The guarded
// discard window forbids writes to the data volume, so the ranges and the
// per-command results stay in caller-owned RAM until the filesystem is
// mounted, then render as JSON lines. The console gets one summary line per
// ext2 block group, capped, instead of one line per range.

#include "ssd_trim_plan.h"

#include <stddef.h>
#include <stdint.h>

namespace brook {

constexpr uint32_t SsdTrimLogSectorsPerGroup = 8u * 32768u; // 4 KiB blocks, 32768 per group
constexpr uint32_t SsdTrimConsoleGroupLines = 64;
// Bound for one rendered JSONL line, including the worst-case summary.
constexpr size_t SsdTrimLogLineMax = 640;

struct SsdTrimLogRange {
    uint64_t lba;
    uint64_t sectors;
    uint32_t batch;
};

struct SsdTrimLogCommand {
    uint64_t ns;
    uint32_t batch;
    uint16_t payloadBlocks;
    bool submitted;
    bool ok;
};

// Fixed capacity; entries past capacity are counted, never silently lost.
struct SsdTrimLog {
    SsdTrimLogRange* ranges = nullptr;
    uint32_t rangeCapacity = 0, rangeCount = 0;
    SsdTrimLogCommand* commands = nullptr;
    uint32_t commandCapacity = 0, commandCount = 0;
    uint64_t rangesDropped = 0, commandsDropped = 0;
    uint32_t batch = 0; // batch the next range or command belongs to

    void AddRange(TrimRange range)
    {
        if (rangeCount < rangeCapacity && ranges)
            ranges[rangeCount++] = {range.lba, range.sectors, batch};
        else
            ++rangesDropped;
    }
    void AddCommand(uint16_t payloadBlocks, uint64_t ns, bool submitted, bool ok)
    {
        if (commandCount < commandCapacity && commands)
            commands[commandCount++] = {ns, batch, payloadBlocks, submitted, ok};
        else
            ++commandsDropped;
    }
    void EndBatch() { ++batch; }
};

inline uint32_t SsdTrimGroupOf(uint64_t partitionLba, uint64_t lba)
{
    return lba < partitionLba ? 0 : uint32_t((lba - partitionLba) / SsdTrimLogSectorsPerGroup);
}

struct SsdTrimGroupSummary {
    uint32_t group = 0;
    uint32_t ranges = 0;
    uint64_t sectors = 0;
};

// Ranges arrive in ascending group order from Ext2TrimScanClean. A range
// never spans groups because free runs are found within one group bitmap.
class SsdTrimGroupAccumulator {
public:
    explicit SsdTrimGroupAccumulator(uint64_t partitionLba, uint32_t lineCap = SsdTrimConsoleGroupLines)
        : partitionLba_(partitionLba), lineCap_(lineCap) {}

    template<class Emit>
    void Add(TrimRange range, Emit emit)
    {
        const uint32_t group = SsdTrimGroupOf(partitionLba_, range.lba);
        if (current_.ranges && group != current_.group)
            Flush(emit);
        if (!current_.ranges)
            current_.group = group;
        ++current_.ranges;
        current_.sectors += range.sectors;
    }

    template<class Emit>
    void Finish(Emit emit)
    {
        if (current_.ranges)
            Flush(emit);
    }

    uint32_t groups() const { return groups_; }
    uint32_t printed() const { return printed_; }
    uint32_t suppressedGroups() const { return groups_ - printed_; }
    uint64_t suppressedRanges() const { return suppressedRanges_; }
    uint64_t suppressedSectors() const { return suppressedSectors_; }

private:
    template<class Emit>
    void Flush(Emit emit)
    {
        ++groups_;
        if (printed_ < lineCap_) {
            ++printed_;
            emit(current_);
        } else {
            suppressedRanges_ += current_.ranges;
            suppressedSectors_ += current_.sectors;
        }
        current_ = {};
    }

    uint64_t partitionLba_;
    uint32_t lineCap_;
    SsdTrimGroupSummary current_{};
    uint32_t groups_ = 0, printed_ = 0;
    uint64_t suppressedRanges_ = 0, suppressedSectors_ = 0;
};

// Bounded text builder; a line that does not fit is reported as overflow
// rather than truncated.
struct SsdTrimText {
    char* out;
    size_t capacity;
    size_t used = 0;
    bool overflow = false;

    void Str(const char* s)
    {
        while (*s) Char(*s++);
    }
    void Char(char c)
    {
        if (used + 1 >= capacity) { overflow = true; return; }
        out[used++] = c;
        out[used] = 0;
    }
    void U64(uint64_t value)
    {
        char digits[20];
        int n = 0;
        do { digits[n++] = char('0' + value % 10); value /= 10; } while (value);
        while (n) Char(digits[--n]);
    }
    void I64(int64_t value)
    {
        if (value < 0) { Char('-'); U64(uint64_t(-(value + 1)) + 1); }
        else U64(uint64_t(value));
    }
    // Every record opens with its "type" key, so fields always follow a comma.
    void Field(const char* name, uint64_t value)
    {
        Str(",\""); Str(name); Str("\":"); U64(value);
    }
};

struct SsdTrimLogHeader {
    bool run = false;
    uint32_t minimumMiB = 0, maximumMiB = 0;
    uint64_t partitionLba = 0, partitionSectors = 0;
    uint64_t startedUnixNs = 0;
};

struct SsdTrimLogResult {
    uint32_t ranges = 0, dsmCommands = 0, plannedCommands = 0;
    uint64_t bytes = 0, commandTotalNs = 0, commandMaxNs = 0, durationNs = 0;
    int error = 0;
};

// Renders line `index` of the JSONL record into out (with trailing '\n').
// Line 0 is the header, then every range, every command and a final summary.
// Returns false past the last line or when the line does not fit.
inline bool SsdTrimLogLine(const SsdTrimLog& log, const SsdTrimLogHeader& header,
                           const SsdTrimLogResult& result, uint32_t index,
                           char* out, size_t capacity)
{
    if (!out || capacity < 2)
        return false;
    out[0] = 0;
    SsdTrimText t{out, capacity};
    const uint64_t total = 2ull + log.rangeCount + log.commandCount;
    if (index >= total)
        return false;
    if (index == 0) {
        t.Str("{\"type\":\"header\",\"version\":1,\"mode\":\"");
        t.Str(header.run ? "run" : "dry-run");
        t.Char('"');
        t.Field("min_mib", header.minimumMiB);
        t.Field("max_mib", header.maximumMiB);
        t.Field("partition_lba", header.partitionLba);
        t.Field("partition_sectors", header.partitionSectors);
        t.Field("started_unix_ns", header.startedUnixNs);
    } else if (index <= log.rangeCount) {
        const auto& r = log.ranges[index - 1];
        t.Str("{\"type\":\"range\"");
        t.Field("i", index - 1);
        t.Field("lba", r.lba);
        t.Field("sectors", r.sectors);
        t.Field("group", SsdTrimGroupOf(header.partitionLba, r.lba));
        t.Field("batch", r.batch);
    } else if (index <= uint64_t(log.rangeCount) + log.commandCount) {
        const uint32_t i = index - 1 - log.rangeCount;
        const auto& c = log.commands[i];
        t.Str("{\"type\":\"command\"");
        t.Field("i", i);
        t.Field("batch", c.batch);
        t.Field("payload_blocks", c.payloadBlocks);
        t.Field("ns", c.ns);
        t.Str(",\"submitted\":"); t.Str(c.submitted ? "true" : "false");
        t.Str(",\"ok\":"); t.Str(c.ok ? "true" : "false");
    } else {
        t.Str("{\"type\":\"summary\",\"mode\":\"");
        t.Str(header.run ? "run" : "dry-run");
        t.Char('"');
        t.Field("ranges", result.ranges);
        t.Field("bytes", result.bytes);
        t.Field("dsm_commands", result.dsmCommands);
        t.Field("planned_commands", result.plannedCommands);
        t.Field("command_total_ns", result.commandTotalNs);
        t.Field("command_max_ns", result.commandMaxNs);
        t.Field("duration_ns", result.durationNs);
        t.Str(",\"error\":"); t.I64(result.error);
        t.Field("ranges_logged", log.rangeCount);
        t.Field("ranges_dropped", log.rangesDropped);
        t.Field("commands_logged", log.commandCount);
        t.Field("commands_dropped", log.commandsDropped);
    }
    t.Str("}\n");
    return !t.overflow;
}

inline uint64_t SsdTrimLogLineCount(const SsdTrimLog& log)
{
    return 2ull + log.rangeCount + log.commandCount;
}

} // namespace brook
