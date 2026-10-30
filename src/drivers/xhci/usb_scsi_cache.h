#pragma once
#include <stdint.h>
namespace brook {
enum class UsbScsiCache : uint8_t { Unknown, WriteThrough, WriteBack };
struct UsbScsiSense {bool valid=false,current=false;uint8_t key=0,asc=0,ascq=0;};
inline UsbScsiSense UsbScsiParseSense(const uint8_t* data,uint32_t bytes) {
    UsbScsiSense result{};if(!data||!bytes)return result;
    unsigned format=data[0]&0x7f;
    if(format==0x70||format==0x71) {
        if(bytes<14||data[7]<6)return result;
        result.key=data[2]&15;result.asc=data[12];result.ascq=data[13];
    }else if(format==0x72||format==0x73) {
        if(bytes<8)return result;
        result.key=data[1]&15;result.asc=data[2];result.ascq=data[3];
    }else return result;
    result.valid=true;result.current=(format==0x70||format==0x72);return result;
}
inline UsbScsiCache UsbScsiModeCache6(const uint8_t* data,uint32_t bytes) {
    if(!data||bytes<4)return UsbScsiCache::Unknown;
    const uint32_t total=uint32_t(data[0])+1;
    if(total<4||total>bytes||uint32_t(data[3])>total-4)return UsbScsiCache::Unknown;
    uint32_t offset=4+data[3];UsbScsiCache found=UsbScsiCache::Unknown;
    while(offset<total) {
        if(total-offset<2||(data[offset]&0x40))return UsbScsiCache::Unknown; // unsupported subpage encoding
        uint32_t length=uint32_t(data[offset+1])+2;
        if(length>total-offset)return UsbScsiCache::Unknown;
        if((data[offset]&0x3f)==8) {
            // Require the complete declared standard caching page, with at
            // least the original SCSI caching fields, not merely a WCE byte.
            if(length<12||found!=UsbScsiCache::Unknown)return UsbScsiCache::Unknown;
            found=(data[offset+2]&4)?UsbScsiCache::WriteBack:UsbScsiCache::WriteThrough;
        }
        offset+=length;
    }
    return found;
}
inline bool UsbScsiUnsupportedSync(const UsbScsiSense& sense) {
    return sense.valid&&sense.current&&sense.key==5&&sense.asc==0x20&&sense.ascq==0;
}
inline bool UsbScsiWriteThroughFlush(UsbScsiCache cache,const UsbScsiSense& sense) {
    return cache==UsbScsiCache::WriteThrough&&UsbScsiUnsupportedSync(sense);
}
inline bool UsbScsiShortInCompletion(bool input,bool singleDataTrb,bool allowed,
                                    uint32_t requested,uint32_t cc,uint32_t residue) {
    if(cc==1)return residue==0;
    return input&&singleDataTrb&&allowed&&cc==13&&residue<=requested;
}
}
