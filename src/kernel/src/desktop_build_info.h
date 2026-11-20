#pragma once
#include "desktop_calendar.h"
#include <stdint.h>

namespace brook {

inline constexpr unsigned BuildIdCapacity = 96;
inline constexpr unsigned BuildCommitCapacity = 65;
inline constexpr unsigned BuildProvenanceLimit = 128;

enum class BuildTimestampSource : uint8_t { Unknown, Clock, SourceDateEpoch };

struct DesktopBuildProvenance {
    char assemblyCommit[BuildCommitCapacity];
    char assembledUtc[21];
    BuildTimestampSource timestampSource;
    bool dirty;
};

struct DesktopBuildInfo {
    char id[BuildIdCapacity];
    char kernelCommit[BuildCommitCapacity];
    DesktopBuildProvenance initrd;
    uint64_t generation, uptimeSeconds;
    bool kernelDirty;
};

inline bool DesktopBuildCommitValid(const char* text, unsigned length) {
    if (length != 40 && length != 64) return false;
    for (unsigned i = 0; i < length; ++i) {
        const char c = text[i];
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) return false;
    }
    return true;
}

inline bool DesktopBuildTimestampValid(const char* text, unsigned length) {
    if (length != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != 'Z') return false;
    const auto number = [&](unsigned start, unsigned count) {
        unsigned value = 0;
        for (unsigned i = start; i < start + count; ++i) {
            if (text[i] < '0' || text[i] > '9') return 10000u;
            value = value * 10 + unsigned(text[i] - '0');
        }
        return value;
    };
    const unsigned year = number(0, 4), month = number(5, 2), day = number(8, 2);
    return year <= 9999 && month <= 12 && day <= 31 &&
           CalendarDateValid({{uint16_t(year), uint8_t(month)}, uint8_t(day)}) &&
           number(11, 2) < 24 && number(14, 2) < 60 && number(17, 2) < 60;
}

inline bool ParseDesktopBuildProvenance(const char* bytes, unsigned length, DesktopBuildProvenance& out) {
    out = {};
    if (!bytes || !length || length > BuildProvenanceLimit || bytes[length - 1] != '\n') return false;

    struct Line {
        const char* text;
        unsigned length;
        bool Is(const char* literal) const {
            unsigned i = 0;
            while (i < length && literal[i] && text[i] == literal[i]) ++i;
            return i == length && !literal[i];
        }
    };
    unsigned pos = 0;
    const auto line = [&]() {
        const unsigned start = pos;
        while (pos < length && bytes[pos] != '\n') ++pos;
        const Line result{bytes + start, pos - start};
        if (pos < length) ++pos;
        return result;
    };

    if (!line().Is("BROOK_BUILD_V1")) return false;
    const auto commit = line(), stamp = line(), timeSource = line(), tree = line();
    if (pos != length || !DesktopBuildCommitValid(commit.text, commit.length) ||
        !DesktopBuildTimestampValid(stamp.text, stamp.length) ||
        (!tree.Is("clean") && !tree.Is("dirty"))) return false;
    if (timeSource.Is("clock")) out.timestampSource = BuildTimestampSource::Clock;
    else if (timeSource.Is("source-date-epoch")) out.timestampSource = BuildTimestampSource::SourceDateEpoch;
    else return false;
    for (unsigned i = 0; i < commit.length; ++i) out.assemblyCommit[i] = commit.text[i];
    for (unsigned i = 0; i < stamp.length; ++i) out.assembledUtc[i] = stamp.text[i];
    out.dirty = tree.Is("dirty");
    return true;
}

} // namespace brook
