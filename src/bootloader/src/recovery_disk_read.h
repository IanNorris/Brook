#pragma once
#include "recovery_disk_gate.h"

namespace brook::bootloader {
enum class RecoveryDiskState { Invalid, NeedsCheck, Clean };

// Reader must provide Read(lba, sectorCount, destination), and Stable(),
// which verifies the originally selected media identity/geometry has not
// changed. sectors must equal the captured parent-media geometry. Read must
// bound requests, transfer the entire request and handle firmware alignment.
// Scratch areas are independent caller-owned buffers, each at least capacity
// bytes and disjoint from all state referenced by Reader; no allocation,
// write, disk selection or repair authority occurs here. Stable metadata is
// not proof that firmware bypassed a physical-media cache.
inline bool RecoveryRangesOverlap(uintptr_t a,uintptr_t as,uintptr_t b,uintptr_t bs) {
    if(a>UINTPTR_MAX-as || b>UINTPTR_MAX-bs)return true;
    return a<b+bs && b<a+as;
}
template<class Reader>
RecoveryDiskState RecoveryReadDiskState(Reader& io, uint64_t sectors,
        const RecoveryDiskIdentity& id, uint8_t* firstEntries,
        uint8_t* lastEntries, uint32_t capacity) {
    const uintptr_t first=reinterpret_cast<uintptr_t>(firstEntries);
    const uintptr_t last=reinterpret_cast<uintptr_t>(lastEntries);
    if (!firstEntries || !lastEntries || !capacity || first>UINTPTR_MAX-capacity ||
        last>UINTPTR_MAX-capacity || RecoveryRangesOverlap(first,capacity,last,capacity) ||
        RecoveryRangesOverlap(first,capacity,reinterpret_cast<uintptr_t>(&id),sizeof(id)) ||
        RecoveryRangesOverlap(last,capacity,reinterpret_cast<uintptr_t>(&id),sizeof(id)) ||
        RecoveryRangesOverlap(first,capacity,reinterpret_cast<uintptr_t>(&io),sizeof(io)) ||
        RecoveryRangesOverlap(last,capacity,reinterpret_cast<uintptr_t>(&io),sizeof(io)) ||
        !io.Stable() || sectors<68)
        return RecoveryDiskState::Invalid;
    uint8_t mbr[512]{},primary[512]{},backup[512]{},superblock[1024]{};
    if (!io.Read(0,1,mbr) || !io.Read(1,1,primary) || !io.Read(sectors-1,1,backup))
        return RecoveryDiskState::Invalid;
    StrictGptHeader a{},b{};
    if (StrictGptCheckHeader(primary,512,sectors,false,a)!=StrictGptResult::Ok ||
        StrictGptCheckHeader(backup,512,sectors,true,b)!=StrictGptResult::Ok)
        return RecoveryDiskState::Invalid;
    const uint64_t bytes=uint64_t(a.count)*a.entrySize;
    const uint64_t blocks=(bytes+511)/512;
    if (!bytes || blocks>capacity/512 || blocks>0xffffffffu ||
        uint64_t(b.count)*b.entrySize!=bytes ||
        !io.Read(a.entriesLba,uint32_t(blocks),firstEntries) ||
        !io.Read(b.entriesLba,uint32_t(blocks),lastEntries))
        return RecoveryDiskState::Invalid;
    RecoveryDataPartition data{};
    if (!RecoveryLocateData(mbr,primary,backup,firstEntries,lastEntries,uint32_t(bytes),sectors,id,data) ||
        data.sectors<4 || !io.Read(data.first+2,2,superblock) || !io.Stable())
        return RecoveryDiskState::Invalid;
    LargeExt2Geometry geometry{};
    const auto state=LargeExt2ClassifyHeader(superblock,sizeof(superblock),data.sectors*512,id.fsUuid,geometry);
    if(state==LargeExt2HeaderState::Clean)return RecoveryDiskState::Clean;
    if(state==LargeExt2HeaderState::NeedsCheck)return RecoveryDiskState::NeedsCheck;
    return RecoveryDiskState::Invalid;
}
} // namespace brook::bootloader
