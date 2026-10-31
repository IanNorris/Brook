#pragma once
#include "strict_gpt_inventory.h"
#include "ahci_write_target.h"

namespace brook {
// A boot record selects exactly one GPT ESP on an already identity-matched
// disk. GUIDs are 32 lowercase hexadecimal digits in their on-disk byte order.
// BRKESP1\n<disk-guid-hex>\n<partition-guid-hex>\n<first-lba>,<sector-count>\n
struct AhciBootSelector { uint8_t diskGuid[16]{},partitionGuid[16]{};uint64_t first=0,count=0; };
inline bool AhciBootGuid(const char*& p,const char* end,uint8_t* out) {
    for(unsigned i=0;i<16;++i) {
        if(end-p<2)return false;
        auto digit=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
        const int a=digit(*p++),b=digit(*p++);if(a<0 || b<0)return false;
        out[i]=uint8_t((a<<4)|b);
    }
    if(p==end || *p++!='\n' || StrictAllZero(out,16))return false;
    return true;
}
inline bool AhciBootSelectorParse(const char* data,uint32_t size,AhciBootSelector& out) {
    out={};if(!data || size<78 || size>128 || memcmp(data,"BRKESP1\n",8))return false;
    AhciBootSelector next{};const char* p=data+8;const char* end=data+size;
    if(!AhciBootGuid(p,end,next.diskGuid) || !AhciBootGuid(p,end,next.partitionGuid) ||
       !AhciTargetNumber(p,end,next.first,',') || !AhciTargetNumber(p,end,next.count,'\n') ||
       p!=end || !next.first || !next.count || next.first>~uint64_t{0}-next.count)return false;
    out=next;return true;
}
struct AhciBootSelection { uint32_t entry=0;uint64_t first=0,count=0; };
// wanted must have parsed successfully. All three sector buffers contain 512
// bytes, and both entry buffers contain bytes bytes read at the respective
// validated headers' entry LBAs. The caller must separately
// establish ATA identity and 512-byte geometry; this does not validate FAT or
// grant write permission.
inline bool AhciBootSelect(const AhciBootSelector& wanted,uint64_t diskSectors,
                           const uint8_t* mbr,const uint8_t* primary,const uint8_t* backup,
                           const uint8_t* entries,const uint8_t* backupEntries,uint32_t bytes,
                           AhciBootSelection& out) {
    out={};StrictGptHeader a{},b{};
    if(StrictGptProtectiveMbr(mbr,diskSectors)!=StrictMbrResult::Protective ||
       StrictGptCheckHeader(primary,512,diskSectors,false,a)!=StrictGptResult::Ok ||
       StrictGptCheckHeader(backup,512,diskSectors,true,b)!=StrictGptResult::Ok ||
       StrictGptCheckCopies(a,entries,b,backupEntries,bytes)!=StrictGptResult::Ok ||
       memcmp(wanted.diskGuid,a.diskGuid,16))return false;
    static constexpr uint8_t esp[16]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b};
    for(uint32_t i=0;i<a.count;++i) {
        const uint8_t* entry=entries+uint64_t(i)*a.entrySize;
        if(memcmp(entry+16,wanted.partitionGuid,16))continue;
        const uint64_t first=StrictLe64(entry+32),last=StrictLe64(entry+40);
        if(memcmp(entry,esp,16) || first!=wanted.first || last-first+1!=wanted.count)return false;
        out.entry=i+1;out.first=first;out.count=last-first+1;return true;
    }
    return false;
}
}
