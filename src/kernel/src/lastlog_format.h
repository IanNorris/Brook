#pragma once
// lastlog_format.h -- fixed-size "last log" records that survive a hang.
//
// Each slot file is exactly LastLogFileBytes: a 64-byte header followed by
// text, zero padded. Files are created once at full size and afterwards only
// overwritten in place, so a running boot never allocates blocks or changes
// the file size. Writers alternate between two slots by sequence parity; a
// torn write fails its CRC and leaves the other slot readable. Sequence
// numbers continue across boots, so the newest valid slot found before the
// first write of a boot is the previous boot's last record. Pure and
// host-testable; the CRC is the panic journal's CRC-32.
#include <stdint.h>
#include "panic_journal.h"

namespace brook {

constexpr uint32_t LastLogFileBytes = 65536;
constexpr uint32_t LastLogHeaderBytes = 64;
constexpr uint32_t LastLogPayloadMax = LastLogFileBytes - LastLogHeaderBytes;
constexpr uint32_t LastLogBootIdBytes = 32;

struct LastLogRecord {
    bool valid = false;
    uint64_t sequence = 0;
    uint32_t payloadBytes = 0;
    uint32_t payloadCrc = 0;
    char bootId[LastLogBootIdBytes + 1] = {};
};

// Layout: magic "BRKLLOG1" [0,8), version=1 [8,12), payload bytes [12,16),
// sequence [16,24), boot id ASCII [24,56), payload CRC [56,60), header CRC
// over bytes [0,60) [60,64).
inline void LastLogEncode(uint8_t* file, uint64_t sequence, const char* bootId,
                          const uint8_t* payload, uint32_t payloadBytes)
{
    if (payloadBytes > LastLogPayloadMax) payloadBytes = LastLogPayloadMax;
    for (uint32_t i = 0; i < LastLogFileBytes; ++i) file[i] = 0;
    const char magic[8] = {'B','R','K','L','L','O','G','1'};
    for (unsigned i = 0; i < 8; ++i) file[i] = static_cast<uint8_t>(magic[i]);
    PanicJournalPut32(file + 8, 1);
    PanicJournalPut32(file + 12, payloadBytes);
    PanicJournalPut64(file + 16, sequence);
    for (uint32_t i = 0; bootId && i < LastLogBootIdBytes && bootId[i]; ++i)
        file[24 + i] = static_cast<uint8_t>(bootId[i]);
    for (uint32_t i = 0; i < payloadBytes; ++i) file[LastLogHeaderBytes + i] = payload[i];
    PanicJournalPut32(file + 56, PanicJournalCrc(file + LastLogHeaderBytes, payloadBytes));
    PanicJournalPut32(file + 60, PanicJournalCrc(file, 60));
}

inline LastLogRecord LastLogDecode(const uint8_t* file, uint32_t bytes)
{
    LastLogRecord r{};
    if (!file || bytes < LastLogHeaderBytes) return r;
    const char magic[8] = {'B','R','K','L','L','O','G','1'};
    for (unsigned i = 0; i < 8; ++i)
        if (file[i] != static_cast<uint8_t>(magic[i])) return r;
    if (PanicJournalGet32(file + 8) != 1) return r;
    if (PanicJournalCrc(file, 60) != PanicJournalGet32(file + 60)) return r;
    const uint32_t payload = PanicJournalGet32(file + 12);
    const uint64_t sequence = PanicJournalGet64(file + 16);
    if (!sequence || payload > LastLogPayloadMax || payload > bytes - LastLogHeaderBytes) return r;
    const uint32_t crc = PanicJournalGet32(file + 56);
    if (PanicJournalCrc(file + LastLogHeaderBytes, payload) != crc) return r;
    r.valid = true;
    r.sequence = sequence;
    r.payloadBytes = payload;
    r.payloadCrc = crc;
    for (uint32_t i = 0; i < LastLogBootIdBytes; ++i) {
        const uint8_t c = file[24 + i];
        r.bootId[i] = (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : 0;
        if (!c) break;
    }
    r.bootId[LastLogBootIdBytes] = 0;
    return r;
}

// Index (0 or 1) of the newest valid slot, or -1. Equal sequences in both
// slots cannot come from one writer and are treated as ambiguous.
inline int LastLogNewest(const LastLogRecord& a, const LastLogRecord& b)
{
    if (a.valid && b.valid) {
        if (a.sequence == b.sequence) return -1;
        return a.sequence > b.sequence ? 0 : 1;
    }
    return a.valid ? 0 : b.valid ? 1 : -1;
}

// The next sequence after everything already on disk (1 on blank media).
inline uint64_t LastLogNextSequence(const LastLogRecord& a, const LastLogRecord& b)
{
    uint64_t top = 0;
    if (a.valid && a.sequence > top) top = a.sequence;
    if (b.valid && b.sequence > top) top = b.sequence;
    return top == ~uint64_t(0) ? top : top + 1;
}

inline uint32_t LastLogSlotFor(uint64_t sequence) { return static_cast<uint32_t>(sequence & 1); }

} // namespace brook
