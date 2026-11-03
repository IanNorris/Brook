#pragma once
#include <stdint.h>

namespace brook {
struct Device;
// Conservative common subset of fsck.fat and the bundled FatFs. This does
// not validate directory entries, cluster chains or grant write authority.
struct GuardFatProfile {
    uint32_t total=0, fatStart=0, fatSectors=0, dataStart=0, clusters=0, rootCluster=0;
    uint16_t rootEntries=0, backupBoot=0;
    uint8_t sectorsPerCluster=0, type=0; // type 16 or 32
};
inline uint16_t GuardFat16(const uint8_t* p) {return uint16_t(p[0])|uint16_t(p[1])<<8;}
inline uint32_t GuardFat32(const uint8_t* p) {return uint32_t(GuardFat16(p))|uint32_t(GuardFat16(p+2))<<16;}
inline bool GuardFatParse(const uint8_t* b,uint32_t size,uint64_t deviceSectors,GuardFatProfile& out) {
    out={};
    if(!b || size<512 || b[510]!=0x55 || b[511]!=0xaa || (b[0]!=0xeb && b[0]!=0xe9) ||
       GuardFat16(b+11)!=512 || b[16]!=2 || b[21]!=0xf8)return false;
    const uint32_t cluster=b[13],reserved=GuardFat16(b+14),root=GuardFat16(b+17);
    const uint32_t total16=GuardFat16(b+19),total32=GuardFat32(b+32);
    const uint32_t total=total16?total16:total32;
    const uint32_t fat16=GuardFat16(b+22),fat32=GuardFat32(b+36);
    const uint32_t fat=fat16?fat16:fat32;
    if(!cluster || cluster>64 || (cluster&(cluster-1)) || !reserved || !total ||
       (total16 && total32) || total!=deviceSectors || !fat || (root%16))return false;
    const uint64_t data=uint64_t(reserved)+uint64_t(fat)*2+root/16;
    if(data>=total)return false;
    const uint32_t count=uint32_t((total-data)/cluster);
    // Exclude counts classified differently by FatFs and standard FAT rules.
    uint8_t type=0;
    if(count>=4086 && count<=65524)type=16;
    else if(count>=65526 && count<=0x0fffffee)type=32;
    else return false;
    if(uint64_t(count+2)*(type==16?2:4)>uint64_t(fat)*512)return false;
    uint16_t backup=0;uint32_t rootCluster=0;
    if(type==16) {
        if(!fat16 || !root || (!total16 && total32<65536))return false;
    } else {
        rootCluster=GuardFat32(b+44);backup=GuardFat16(b+50);
        static constexpr char signature[]="FAT32   ";
        for(unsigned i=0;i<8;++i)if(b[82+i]!=uint8_t(signature[i]))return false;
        if(fat16 || total16 || root || GuardFat16(b+40) || GuardFat16(b+42) ||
           rootCluster<2 || rootCluster>=count+2 || GuardFat16(b+48)!=1 ||
           backup!=6 || backup>=reserved)return false;
    }
    out={total,reserved,fat,uint32_t(data),count,rootCluster,uint16_t(root),backup,uint8_t(cluster),type};
    return true;
}
bool GuardFatCheckDevice(Device* device,GuardFatProfile& profile);
}
