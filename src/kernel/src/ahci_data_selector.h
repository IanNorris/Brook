#pragma once
#include "ahci_boot_selector.h"

namespace brook {
// Prospective direct data store selection. GUIDs are 32 lowercase hex digits
// in their on-disk byte order. No I/O, mount or write authority is granted.
// BRKDATA1\n<disk-guid>\n<partition-guid>\n<first>,<count>\n<filesystem-uuid>\n
struct AhciDataSelector {
    uint8_t diskGuid[16]{}, partitionGuid[16]{}, filesystemUuid[16]{};
    uint64_t first=0, count=0;
};
inline bool AhciDataSelectorParse(const char* data,uint32_t size,AhciDataSelector& out) {
    out={};
    if(!data || size<110 || size>160 || memcmp(data,"BRKDATA1\n",9))return false;
    AhciDataSelector next{};const char* p=data+9;const char* end=data+size;
    if(!AhciBootGuid(p,end,next.diskGuid) || !AhciBootGuid(p,end,next.partitionGuid) ||
       !AhciTargetNumber(p,end,next.first,',') || !AhciTargetNumber(p,end,next.count,'\n') ||
       !AhciBootGuid(p,end,next.filesystemUuid) || p!=end || !next.first || !next.count ||
       next.first>~uint64_t{0}-next.count || next.first>~uint64_t{0}/512 ||
       next.count>~uint64_t{0}/512)return false;
    out=next;return true;
}

// The caller must separately verify ATA identity and that the boot ESP was
// selected from the same disk/GPT snapshot. Filesystem UUID and bytes require
// separate validation against the selected partition before any writable use.
inline bool AhciDataSelect(const AhciDataSelector& wanted,uint64_t diskSectors,
                           const uint8_t* mbr,const uint8_t* primary,const uint8_t* backup,
                           const uint8_t* entries,const uint8_t* backupEntries,uint32_t bytes,
                           AhciBootSelection& out) {
    out={};StrictGptHeader a{},b{};
    if(StrictAllZero(wanted.diskGuid,16) || StrictAllZero(wanted.partitionGuid,16) ||
       StrictAllZero(wanted.filesystemUuid,16) || !wanted.first || !wanted.count ||
       wanted.first>~uint64_t{0}-wanted.count || wanted.first>~uint64_t{0}/512 ||
       wanted.count>~uint64_t{0}/512 || diskSectors>~uint64_t{0}/512 ||
       StrictGptProtectiveMbr(mbr,diskSectors)!=StrictMbrResult::Protective ||
       StrictGptCheckHeader(primary,512,diskSectors,false,a)!=StrictGptResult::Ok ||
       StrictGptCheckHeader(backup,512,diskSectors,true,b)!=StrictGptResult::Ok ||
       StrictGptCheckCopies(a,entries,b,backupEntries,bytes)!=StrictGptResult::Ok ||
       memcmp(wanted.diskGuid,a.diskGuid,16))return false;
    static constexpr uint8_t linuxFs[16]={0x0f,0xc6,0x3d,0xaf,0x83,0x84,0x72,0x47,
                                           0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
    for(uint32_t i=0;i<a.count;++i) {
        const uint8_t* entry=entries+uint64_t(i)*a.entrySize;
        if(memcmp(entry+16,wanted.partitionGuid,16))continue;
        const uint64_t first=StrictLe64(entry+32),last=StrictLe64(entry+40);
        if(memcmp(entry,linuxFs,16) || first!=wanted.first || last-first+1!=wanted.count)return false;
        out.entry=i+1;out.first=first;out.count=last-first+1;return true;
    }
    return false;
}
}
