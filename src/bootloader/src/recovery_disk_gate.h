#pragma once

#include "../../kernel/src/strict_gpt_inventory.h"
#include "../../kernel/src/large_ext2_geometry.h"

namespace brook::bootloader {

// Pure validation only: the caller must read both GPT copies and the data
// superblock from the physical parent of the firmware-loaded ESP. A successful
// result does not reserve a disk or grant any write/repair authority.
struct RecoveryDiskIdentity {
    uint8_t diskGuid[16], espGuid[16], dataGuid[16], fsUuid[16];
    uint64_t espFirst, espLast, dataFirst, dataSectors;
    uint32_t espPartitionNumber;
};
struct RecoveryDataPartition { uint64_t first = 0, sectors = 0; };

inline bool RecoveryGuidEqual(const uint8_t* a, const uint8_t* b) {
    if (!a || !b) return false;
    for (unsigned i=0;i<16;++i) if (a[i]!=b[i]) return false;
    return true;
}

// The caller must first validate both headers to learn bounded array lengths;
// this routine repeats validation and compares all provided bytes before use.
inline bool RecoveryLocateData(const uint8_t* mbr, const uint8_t* primary,
        const uint8_t* backup, const uint8_t* primaryEntries,
        const uint8_t* backupEntries, uint32_t entryBytes, uint64_t sectors,
        const RecoveryDiskIdentity& id, RecoveryDataPartition& result) {
    result = {};
    if (StrictGptProtectiveMbr(mbr,sectors)!=StrictMbrResult::Protective ||
        StrictAllZero(id.diskGuid,16) || StrictAllZero(id.espGuid,16) ||
        StrictAllZero(id.dataGuid,16) || id.espFirst>id.espLast ||
        !id.espPartitionNumber || id.dataSectors<4) return false;
    StrictGptHeader a{},b{};
    if (StrictGptCheckHeader(primary,512,sectors,false,a)!=StrictGptResult::Ok ||
        StrictGptCheckHeader(backup,512,sectors,true,b)!=StrictGptResult::Ok ||
        StrictGptCheckCopies(a,primaryEntries,b,backupEntries,entryBytes)!=StrictGptResult::Ok ||
        !RecoveryGuidEqual(a.diskGuid,id.diskGuid)) return false;
    static constexpr uint8_t espType[16]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b};
    static constexpr uint8_t dataType[16]={0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
    bool esp=false,data=false;
    RecoveryDataPartition found{};
    for (uint32_t i=0;i<a.count;++i) {
        const uint8_t* e=primaryEntries+uint64_t(i)*a.entrySize;
        if (RecoveryGuidEqual(e+16,id.espGuid)) {
            if (esp || i+1!=id.espPartitionNumber || !RecoveryGuidEqual(e,espType) || StrictLe64(e+32)!=id.espFirst ||
                StrictLe64(e+40)!=id.espLast) return false;
            esp=true;
        }
        if (RecoveryGuidEqual(e+16,id.dataGuid)) {
            if (data || !RecoveryGuidEqual(e,dataType)) return false;
            const uint64_t first=StrictLe64(e+32),last=StrictLe64(e+40);
            if (last<first || last-first==~uint64_t{0} || first!=id.dataFirst ||
                last-first+1!=id.dataSectors) return false;
            found={first,last-first+1};data=true;
        }
    }
    if (!esp || !data || found.sectors>~uint64_t{0}/512) return false;
    result=found;return true;
}

inline bool RecoveryDataHeaderIsClean(const uint8_t* superblock,uint32_t size,
        const RecoveryDiskIdentity& id,const RecoveryDataPartition& data) {
    if (!data.sectors || data.sectors>~uint64_t{0}/512) return false;
    LargeExt2Geometry geometry{};
    return LargeExt2ClassifyHeader(superblock,size,data.sectors*512,id.fsUuid,geometry)
        ==LargeExt2HeaderState::Clean;
}

} // namespace brook::bootloader
