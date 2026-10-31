#pragma once
#include "ssd_install_plan.h"
#include "strict_gpt_inventory.h"
#include <string.h>

namespace brook {
// Source buffers must have been read completely from an immutable pinned
// image at their GPT-declared locations. This validates metadata/layout only;
// target identity, source ownership, content authentication and exclusive
// install authority are separate mandatory admission checks.
inline bool SsdInstallPreflight(const SsdInstallGeometry& geometry,uint64_t sourceSectors,
                                uint64_t targetSectors,uint32_t sectorBytes,
                                const uint8_t* mbr,const uint8_t* primary,const uint8_t* backup,
                                const uint8_t* entries,const uint8_t* backupEntries,uint32_t bytes) {
    if(!SsdInstallGeometryValid(geometry) || sourceSectors!=geometry.diskSectors ||
       targetSectors!=geometry.diskSectors || sectorBytes!=512 || bytes!=16384 ||
       !mbr || !primary || !backup || !entries || !backupEntries)return false;
    StrictGptHeader a{},b{};
    if(StrictGptProtectiveMbr(mbr,sourceSectors)!=StrictMbrResult::Protective ||
       StrictGptCheckHeader(primary,512,sourceSectors,false,a)!=StrictGptResult::Ok ||
       StrictGptCheckHeader(backup,512,sourceSectors,true,b)!=StrictGptResult::Ok ||
       a.count!=128 || a.entrySize!=128 || a.entriesLba!=2 || b.entriesLba!=sourceSectors-33 ||
       StrictGptCheckCopies(a,entries,b,backupEntries,bytes)!=StrictGptResult::Ok)return false;
    static constexpr uint8_t esp[16]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0,0xa0,0xc9,0x3e,0xc9,0x3b};
    unsigned partitions=0;
    for(unsigned i=0;i<128;++i) {
        const uint8_t* e=entries+i*128;if(StrictAllZero(e,16))continue;
        ++partitions;const uint64_t first=StrictLe64(e+32),last=StrictLe64(e+40);
        if(memcmp(e,esp,16) || first!=geometry.payloadFirst || last-first+1!=geometry.payloadSectors)return false;
    }
    return partitions==1;
}
}
