#pragma once

#include "payload.h"
#include "../boot_protocol/boot_protocol.h"

namespace brook {

constexpr unsigned GuardSlotSectorBytes = 512;
constexpr unsigned GuardSlotCount = 2;
constexpr unsigned GuardSlotFailureLimit = 2;
constexpr unsigned GuardSlotCrcOffset = GuardSlotSectorBytes - 4;
constexpr uint8_t GuardSlotMagic[] = {'B','R','K','S','L','T','0','1'};

struct GuardSlotHealthEntry {
    uint64_t generation = 0;
    uint8_t selectorDigest[32]{};
    uint32_t starts = 0;
    bool profileRejected = false;
};

struct GuardSlotHealth {
    uint64_t sequence = 0;
    GuardSlotHealthEntry slots[GuardSlotCount]{};
    uint64_t preferredAtGeneration = 0;
    uint32_t preferredSlot = 0;
};

inline bool GuardSlotHasDigest(const uint8_t (&digest)[32])
{
    uint8_t combined = 0;
    for (const auto byte : digest) combined |= byte;
    return combined != 0;
}

inline bool GuardSlotKeyValid(const BootGuardSlot& key)
{
    return key.generation && key.slot < GuardSlotCount && !key.reserved &&
           GuardSlotHasDigest(key.selectorDigest);
}

inline bool GuardSlotMatches(const GuardSlotHealth& health, const BootGuardSlot& key)
{
    if (!GuardSlotKeyValid(key)) return false;
    const auto& entry = health.slots[key.slot];
    return entry.generation == key.generation &&
           BrookUpdateEqual(entry.selectorDigest, key.selectorDigest, sizeof(entry.selectorDigest));
}

inline GuardSlotHealthEntry& GuardSlotRemember(GuardSlotHealth& health, const BootGuardSlot& key)
{
    auto& entry = health.slots[key.slot];
    if (!GuardSlotMatches(health,key)) {
        entry = {};
        entry.generation = key.generation;
        for (unsigned i = 0; i < sizeof(entry.selectorDigest); ++i)
            entry.selectorDigest[i] = key.selectorDigest[i];
    }
    return entry;
}

inline bool GuardSlotStart(GuardSlotHealth& health, const BootGuardSlot& key)
{
    if (!GuardSlotKeyValid(key)) return false;
    auto& entry = GuardSlotRemember(health,key);
    if (entry.profileRejected || entry.starts >= GuardSlotFailureLimit) return false;
    if (key.generation > health.preferredAtGeneration) {
        health.preferredAtGeneration = 0;
        health.preferredSlot = 0;
    }
    ++entry.starts;
    return true;
}

// Only a verified recovery request may clear the selected slot's count.
// The preference expires as soon as a new selector generation is published.
inline bool GuardSlotPrefer(GuardSlotHealth& health, const BootGuardSlot& key, uint64_t newestGeneration)
{
    if (!GuardSlotKeyValid(key) || key.generation > newestGeneration) return false;
    auto& entry = GuardSlotRemember(health,key);
    entry.starts = 0;
    entry.profileRejected = false;
    health.preferredAtGeneration = newestGeneration;
    health.preferredSlot = key.slot;
    return true;
}

inline bool GuardSlotFinish(GuardSlotHealth& health, const BootGuardSlot& key, bool rejected)
{
    if (!GuardSlotMatches(health,key)) return false;
    auto& entry = health.slots[key.slot];
    if (!entry.starts || entry.profileRejected) return false;
    if (rejected) entry.profileRejected = true;
    else entry.starts = 0;
    return true;
}

enum class GuardSlotChoice { Newest, Previous, Recovery };

inline GuardSlotChoice GuardChooseSlot(const GuardSlotHealth& health, const BootGuardSlot& newest,
                                       const BootGuardSlot& previous)
{
    if (health.preferredAtGeneration && health.preferredAtGeneration == newest.generation) {
        const bool useNewest = health.preferredSlot == newest.slot;
        const auto& selected = useNewest ? newest : previous;
        if (selected.slot != health.preferredSlot || !GuardSlotMatches(health,selected) ||
            (!useNewest && selected.generation >= newest.generation))
            return GuardSlotChoice::Recovery;
        const auto& entry = health.slots[selected.slot];
        if (entry.profileRejected || entry.starts >= GuardSlotFailureLimit)
            return GuardSlotChoice::Recovery;
        return useNewest ? GuardSlotChoice::Newest : GuardSlotChoice::Previous;
    }
    if (!GuardSlotMatches(health,newest)) return GuardSlotChoice::Newest;
    const auto& active = health.slots[newest.slot];
    if (active.profileRejected) return GuardSlotChoice::Recovery;
    if (active.starts < GuardSlotFailureLimit) return GuardSlotChoice::Newest;

    if (!GuardSlotKeyValid(previous) || previous.slot == newest.slot ||
        previous.generation >= newest.generation) return GuardSlotChoice::Recovery;
    if (GuardSlotMatches(health,previous)) {
        const auto& older = health.slots[previous.slot];
        if (older.profileRejected || older.starts >= GuardSlotFailureLimit) return GuardSlotChoice::Recovery;
    }
    return GuardSlotChoice::Previous;
}

enum class GuardSlotRecordKind { Blank, Torn, Foreign, Valid };

inline GuardSlotRecordKind GuardSlotDecode(const uint8_t* sector, const uint8_t (&disk)[32],
                                            GuardSlotHealth& result)
{
    bool blank = true;
    for (unsigned i = 0; i < GuardSlotSectorBytes; ++i) blank &= !sector[i];
    if (blank) return GuardSlotRecordKind::Blank;
    if (!BrookUpdateEqual(sector,GuardSlotMagic,sizeof(GuardSlotMagic)))
        return GuardSlotRecordKind::Foreign;
    if (BrookUpdateGet32(sector+GuardSlotCrcOffset) != BrookUpdateCrc(sector,GuardSlotCrcOffset))
        return GuardSlotRecordKind::Torn;
    if (!BrookUpdateEqual(sector+16,disk,sizeof(disk))) return GuardSlotRecordKind::Foreign;

    GuardSlotHealth health{};
    health.sequence = BrookUpdateGet64(sector+8);
    if (!health.sequence) return GuardSlotRecordKind::Foreign;
    for (unsigned index = 0; index < GuardSlotCount; ++index) {
        const auto* bytes = sector+48+index*48;
        auto& entry = health.slots[index];
        entry.generation = BrookUpdateGet64(bytes);
        for (unsigned i = 0; i < sizeof(entry.selectorDigest); ++i) entry.selectorDigest[i] = bytes[8+i];
        entry.starts = BrookUpdateGet32(bytes+40);
        const uint32_t rejected = BrookUpdateGet32(bytes+44);
        if (entry.starts > GuardSlotFailureLimit || rejected > 1 || (rejected && !entry.starts) ||
            (bool(entry.generation) != GuardSlotHasDigest(entry.selectorDigest)) ||
            (!entry.generation && entry.starts)) return GuardSlotRecordKind::Foreign;
        entry.profileRejected = rejected != 0;
    }
    constexpr unsigned offset = 48+GuardSlotCount*48;
    health.preferredAtGeneration = BrookUpdateGet64(sector+offset);
    health.preferredSlot = BrookUpdateGet32(sector+offset+8);
    if (health.preferredSlot >= GuardSlotCount ||
        (!health.preferredAtGeneration && health.preferredSlot) ||
        (health.preferredAtGeneration && (!health.slots[health.preferredSlot].generation ||
            health.slots[health.preferredSlot].generation > health.preferredAtGeneration)))
        return GuardSlotRecordKind::Foreign;
    for (unsigned i = offset+12; i < GuardSlotCrcOffset; ++i)
        if (sector[i]) return GuardSlotRecordKind::Foreign;
    result = health;
    return GuardSlotRecordKind::Valid;
}

struct GuardSlotJournal {
    GuardSlotHealth health{};
    int newest = -1;
};

template<class Io>
bool GuardSlotRead(Io& io, const uint8_t (&disk)[32], GuardSlotJournal& journal)
{
    journal = {};
    bool torn = false;
    for (unsigned index = 0; index < GuardSlotCount; ++index) {
        uint8_t sector[GuardSlotSectorBytes];
        if (!io.read(index,sector)) return false;
        GuardSlotHealth health{};
        const auto kind = GuardSlotDecode(sector,disk,health);
        if (kind == GuardSlotRecordKind::Foreign) return false;
        torn |= kind == GuardSlotRecordKind::Torn;
        if (kind != GuardSlotRecordKind::Valid) continue;
        if (health.sequence == journal.health.sequence) return false;
        if (health.sequence > journal.health.sequence) {
            journal.health = health;
            journal.newest = int(index);
        }
    }
    return journal.newest >= 0 || !torn;
}

template<class Io>
bool GuardSlotWrite(Io& io, const uint8_t (&disk)[32], const GuardSlotHealth& next)
{
    GuardSlotJournal current;
    if (!GuardSlotRead(io,disk,current) || next.sequence != current.health.sequence ||
        next.sequence == ~uint64_t{0}) return false;

    uint8_t sector[GuardSlotSectorBytes]{};
    for (unsigned i = 0; i < sizeof(GuardSlotMagic); ++i) sector[i] = GuardSlotMagic[i];
    BrookUpdatePut64(sector+8,next.sequence+1);
    for (unsigned i = 0; i < sizeof(disk); ++i) sector[16+i] = disk[i];
    for (unsigned index = 0; index < GuardSlotCount; ++index) {
        const auto& entry = next.slots[index];
        auto* bytes = sector+48+index*48;
        BrookUpdatePut64(bytes,entry.generation);
        for (unsigned i = 0; i < sizeof(entry.selectorDigest); ++i) bytes[8+i] = entry.selectorDigest[i];
        BrookUpdatePut32(bytes+40,entry.starts);
        BrookUpdatePut32(bytes+44,entry.profileRejected ? 1 : 0);
    }
    constexpr unsigned offset = 48+GuardSlotCount*48;
    BrookUpdatePut64(sector+offset,next.preferredAtGeneration);
    BrookUpdatePut32(sector+offset+8,next.preferredSlot);
    BrookUpdatePut32(sector+GuardSlotCrcOffset,BrookUpdateCrc(sector,GuardSlotCrcOffset));
    GuardSlotHealth verified{};
    if (GuardSlotDecode(sector,disk,verified) != GuardSlotRecordKind::Valid) return false;

    const unsigned target = current.newest < 0 ? 0 : unsigned(1-current.newest);
    if (!io.write(target,sector) || !io.flush()) return false;
    uint8_t readback[GuardSlotSectorBytes];
    return io.read(target,readback) && BrookUpdateEqual(sector,readback,sizeof(sector));
}
}
