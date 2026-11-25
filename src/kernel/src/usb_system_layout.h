#pragma once

#include "strict_gpt_inventory.h"

namespace brook {
struct UsbSystemRecord {
    uint8_t guid[16]{};
    uint64_t first=0, sectors=0;
    uint8_t uuid[16]{};
};

inline bool UsbSystemHex(const char*& p,const char* end,uint8_t (&id)[16]) {
    for(auto& byte:id) {
        byte=0;
        for(unsigned i=0;i<2;++i) {
            if(p==end)return false;
            const char c=*p++;
            if(c>='0' && c<='9')byte=(byte<<4)|(c-'0');
            else if(c>='a' && c<='f')byte=(byte<<4)|(c-'a'+10);
            else return false;
        }
    }
    return !StrictAllZero(id,16);
}

inline bool UsbSystemLba(const char*& p,const char* end,uint64_t& result) {
    result=0;
    while(p!=end && *p>='0' && *p<='9') {
        result=result*10+(*p++-'0');
        if(result>0xffffffffu)return false;
    }
    return result!=0;
}

inline bool UsbSystemParse(const char* p,uint32_t size,UsbSystemRecord& out) {
    constexpr char magic[]="BRKUSB1\n";
    if(!p || size<sizeof(magic)-1)return false;
    const char* const end=p+size;
    for(unsigned i=0;i<sizeof(magic)-1;++i)if(*p++!=magic[i])return false;
    UsbSystemRecord record{};
    if(!UsbSystemHex(p,end,record.guid) || p==end || *p++!='\n' ||
       !UsbSystemLba(p,end,record.first) || p==end || *p++!=',' ||
       !UsbSystemLba(p,end,record.sectors) || p==end || *p++!='\n' ||
       !UsbSystemHex(p,end,record.uuid) || p==end || *p++!='\n' || p!=end ||
       record.first+record.sectors>uint64_t(0xffffffffu)+1 ||
       record.sectors%8)return false;
    out=record;
    return true;
}

inline bool UsbSystemBytesMatch(const uint8_t* a,const uint8_t* b,unsigned count) {
    for(unsigned i=0;i<count;++i)if(a[i]!=b[i])return false;
    return true;
}

inline bool UsbSystemEntryMatches(const uint8_t* entry,const uint8_t* guid,uint64_t first,uint64_t sectors) {
    return !StrictAllZero(entry,16) && UsbSystemBytesMatch(entry+16,guid,16) &&
        StrictLe64(entry+32)==first && sectors && StrictLe64(entry+40)==first+sectors-1;
}

// Both GPT copies and their non-overlapping geometry must have passed
// StrictGptCheckCopies before using these entry identities.
inline bool UsbSystemEntriesMatch(const StrictGptHeader& header,const uint8_t* entries,
        uint32_t bootEntry,const uint8_t* bootGuid,uint64_t bootFirst,uint64_t bootSectors,
        const UsbSystemRecord& record) {
    if(bootEntry!=1 || header.count<2 ||
       !UsbSystemEntryMatches(entries,bootGuid,bootFirst,bootSectors))return false;
    const auto* entry=entries+header.entrySize;
    constexpr uint8_t linuxData[]={0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
    constexpr char name[]="BROOK_SYSTEM";
    if(!UsbSystemBytesMatch(entry,linuxData,16) ||
       !UsbSystemEntryMatches(entry,record.guid,record.first,record.sectors))return false;
    for(unsigned i=0;i<sizeof(name)-1;++i)if(entry[56+i*2]!=name[i] || entry[57+i*2])return false;
    return StrictAllZero(entry+56+2*(sizeof(name)-1),72-2*(sizeof(name)-1));
}
}
