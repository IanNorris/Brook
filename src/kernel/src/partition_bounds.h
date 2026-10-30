#pragma once
#include <stdint.h>
namespace brook {
struct MbrPartition { uint64_t first=0,count=0; };
inline uint32_t MbrRead32(const uint8_t* p) {return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
// Primary data partitions only. Protective/extended layouts are handled by their
// own scanners, never mistaken for a flat filesystem. Return -1 if malformed.
inline int MbrPartitions(const uint8_t* sector,uint64_t diskSectors,MbrPartition* out) {
    if(!sector || !out || sector[510]!=0x55 || sector[511]!=0xaa)return 0;
    unsigned count=0;
    for(unsigned i=0;i<4;++i) {
        const auto* e=sector+446+i*16;
        const uint8_t type=e[4];
        if(!type)continue;
        if(type==0xee || type==5 || type==0xf || type==0x85)return 0;
        if(e[0]!=0 && e[0]!=0x80)return -1;
        const uint64_t first=MbrRead32(e+8),size=MbrRead32(e+12);
        if(!first || !size || first>=diskSectors || size>diskSectors-first)return -1;
        for(unsigned j=0;j<count;++j) if(first<out[j].first+out[j].count && out[j].first<first+size)return -1;
        out[count++]={first,size};
    }
    return count;
}
inline bool PartitionIoSpan(uint64_t start,uint64_t size,uint64_t offset,uint64_t requested,
                            uint64_t& physical,uint64_t& amount) {
    if(offset>size || offset>~uint64_t{0}-start)return false;
    physical=start+offset;amount=requested>size-offset?size-offset:requested;
    return amount<=~uint64_t{0}-physical;
}
}
