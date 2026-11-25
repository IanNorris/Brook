#pragma once
#include "device.h"

namespace brook {
inline uint32_t ScsiUnmapBe32(const uint8_t* p) {
    return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3];
}

inline bool ScsiUnmapVpd(const uint8_t* data,uint32_t bytes,uint8_t page,uint32_t needed) {
    return bytes>=needed && needed>=4 && !(data[0]&0x1f) && data[1]==page &&
        (uint32_t(data[2])<<8 | data[3])>=needed-4;
}

inline uint32_t ScsiEncodeUnmap(uint8_t* list,const UsbUnmapLimits& limits,
    const DeviceWriteWindow* ranges,uint32_t count)
{
    if(!list || !ranges || !count || count>UsbUnmapMaxRanges || count>limits.maxRanges)return 0;
    uint64_t sectors=0;
    for(uint32_t i=0;i<count;++i) {
        const auto& range=ranges[i];
        if(!range.length || ((range.start|range.length)&511) ||
           range.start>~uint64_t{0}-range.length)return 0;
        sectors+=range.length/512;
        if(sectors>limits.maxSectors || sectors>UsbUnmapMaxSectors)return 0;
    }
    const uint32_t bytes=8+16*count;
    for(uint32_t i=0;i<bytes;++i)list[i]=0;
    list[0]=(bytes-2)>>8;list[1]=bytes-2;
    list[2]=(bytes-8)>>8;list[3]=bytes-8;
    for(uint32_t i=0;i<count;++i) {
        uint64_t lba=ranges[i].start/512;
        uint32_t length=ranges[i].length/512;
        for(int j=7;j>=0;--j){list[8+16*i+j]=lba;lba>>=8;}
        for(int j=3;j>=0;--j){list[16+16*i+j]=length;length>>=8;}
    }
    return bytes;
}
}
