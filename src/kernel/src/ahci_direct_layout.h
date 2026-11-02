#pragma once
#include "ahci_data_selector.h"
#include "block_write_leases.h"

namespace brook {
// Boot-time layout from one validated, caller-owned GPT snapshot. Metadata
// ranges conservatively include all sectors outside the usable GPT range.
struct AhciDirectLayout {
    AhciBootSelection esp{},data{};
    uint64_t frontBytes=0,backStart=0,backBytes=0;
};
inline bool AhciDirectLayoutSelect(const AhciBootSelector& boot,const AhciDataSelector& data,
        uint64_t sectors,const uint8_t* mbr,const uint8_t* primary,const uint8_t* backup,
        const uint8_t* entries,const uint8_t* copy,uint32_t bytes,AhciDirectLayout& out) {
    out={};
    if(sectors>~uint64_t{0}/512)return false;
    AhciDirectLayout next{};
    if(!AhciSelectBootAndData(boot,data,sectors,mbr,primary,backup,entries,copy,bytes,next.esp,next.data))return false;
    StrictGptHeader h{};
    if(StrictGptCheckHeader(primary,512,sectors,false,h)!=StrictGptResult::Ok)return false;
    next.frontBytes=h.firstUsable*512;
    next.backStart=(h.lastUsable+1)*512;
    next.backBytes=sectors*512-next.backStart;
    if(!next.frontBytes || !next.backBytes)return false;
    out=next;return true;
}
// Build the complete batch before publishing any lease. Parent and owner are
// pinned device identities; metadata has no writer and the data partition has
// exactly one owner. The ESP is disjoint and remains ordinarily writable.
inline bool AhciDirectLayoutLeases(const AhciDirectLayout& layout,uintptr_t parent,uintptr_t owner,
                                   BlockWriteLease (&leases)[3]) {
    if(!parent || !owner || parent==owner || !layout.esp.count || !layout.data.count ||
       !layout.frontBytes || !layout.backBytes || layout.backStart>~uint64_t{0}-layout.backBytes ||
       layout.data.first>~uint64_t{0}/512 || layout.data.count>~uint64_t{0}/512)return false;
    const uint64_t dataStart=layout.data.first*512, dataBytes=layout.data.count*512;
    if(dataStart<layout.frontBytes || dataStart>layout.backStart || dataBytes>layout.backStart-dataStart)return false;
    leases[0]={parent,0,0,layout.frontBytes};
    leases[1]={parent,owner,dataStart,dataBytes};
    leases[2]={parent,0,layout.backStart,layout.backBytes};
    return true;
}
}
