#pragma once
#include <stdint.h>

namespace brook {
enum class AtaIdentifyResult : uint8_t { Ok, Truncated, Incomplete, PacketDevice, NoLba48, InvalidCapacity, UnsupportedSectorSize };
struct AtaIdentifyInfo {
    uint64_t sectors = 0;
    uint32_t logicalSectorBytes = 0, physicalSectorBytes = 0;
    char model[41]{}, serial[21]{}, firmware[9]{};
};
inline uint16_t AtaIdentifyWord(const uint8_t* data, uint32_t n) {
    return uint16_t(data[n*2]) | (uint16_t(data[n*2+1]) << 8);
}
inline void AtaIdentifyText(const uint8_t* data, uint32_t firstWord, uint32_t words, char* out) {
    uint32_t end=words*2;
    for(uint32_t i=0;i<end;++i) {
        const uint16_t w=AtaIdentifyWord(data,firstWord+i/2);
        const uint8_t c=i%2 ? uint8_t(w) : uint8_t(w>>8);
        out[i]=(c>=32 && c<=126)?char(c):'?';
    }
    while(end && out[end-1]==' ')--end;
    out[end]=0;
}
// Read-only IDENTIFY DEVICE response parser. This first driver supports only
// 512-byte logical sectors and LBA48; it must refuse other geometries.
inline AtaIdentifyResult AtaParseIdentify(const uint8_t* data, uint32_t bytes, AtaIdentifyInfo& out) {
    out={};if(!data || bytes<512)return AtaIdentifyResult::Truncated;
    if(AtaIdentifyWord(data,0)&0x8000u)return AtaIdentifyResult::PacketDevice;
    if(AtaIdentifyWord(data,0)&0x0004u)return AtaIdentifyResult::Incomplete;
    if((AtaIdentifyWord(data,83)&0xc400u)!=0x4400u)return AtaIdentifyResult::NoLba48;
    for(unsigned i=0;i<4;++i)out.sectors|=uint64_t(AtaIdentifyWord(data,100+i))<<(16*i);
    if(!out.sectors || out.sectors>(1ull<<48))return AtaIdentifyResult::InvalidCapacity;
    const uint16_t geometry=AtaIdentifyWord(data,106);
    if((geometry&0xc000u)==0x4000u && (geometry&0x1000u))return AtaIdentifyResult::UnsupportedSectorSize;
    out.logicalSectorBytes=512;out.physicalSectorBytes=512;
    if((geometry&0xc000u)==0x4000u && (geometry&0x2000u))out.physicalSectorBytes=512u<<(geometry&15u);
    AtaIdentifyText(data,10,10,out.serial);AtaIdentifyText(data,23,4,out.firmware);AtaIdentifyText(data,27,20,out.model);
    return AtaIdentifyResult::Ok;
}
} // namespace brook
