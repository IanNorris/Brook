#pragma once
#include <stdint.h>

namespace brook { namespace bootloader {
struct RecoveryEspPath {
    uint32_t partitionNumber = 0;
    uint64_t first = 0, sectors = 0;
    uint8_t guid[16]{};
    uint32_t parentPrefixBytes = 0;
};

// Accept only one complete, bounded path ending in a GPT hard-drive node.
// The caller must bound and safely copy firmware-owned bytes first.
inline bool RecoveryParseEspPath(const uint8_t* path, uint32_t bytes, RecoveryEspPath& out) {
    out = {};
    if (!path || bytes<46 || bytes>4096) return false;
    uint32_t offset=0,nodes=0;
    while (offset+4<=bytes && ++nodes<=128) {
        const uint32_t length=uint32_t(path[offset+2])|(uint32_t(path[offset+3])<<8);
        if (length<4 || length>bytes-offset) return false;
        const uint8_t type=path[offset],subtype=path[offset+1];
        if (type==0x7f) return false; // no end or multi-instance before HD
        if (type==4 && subtype==1) {
            if (!offset || length!=42 || bytes-offset!=46 || path[offset+40]!=2 || path[offset+41]!=2 ||
                path[offset+42]!=0x7f || path[offset+43]!=0xff ||
                path[offset+44]!=4 || path[offset+45]!=0) return false;
            auto get32=[&](uint32_t at){return uint32_t(path[at])|(uint32_t(path[at+1])<<8)|
                (uint32_t(path[at+2])<<16)|(uint32_t(path[at+3])<<24);};
            const uint32_t number=get32(offset+4);
            const uint64_t first=uint64_t(get32(offset+8))|(uint64_t(get32(offset+12))<<32);
            const uint64_t count=uint64_t(get32(offset+16))|(uint64_t(get32(offset+20))<<32);
            uint8_t present=0;for(unsigned i=0;i<16;++i)present|=path[offset+24+i];
            if(!number || !first || !count || !present || count-1>~uint64_t{0}-first) return false;
            out.partitionNumber=number;out.first=first;out.sectors=count;out.parentPrefixBytes=offset;
            for(unsigned i=0;i<16;++i)out.guid[i]=path[offset+24+i];
            return true;
        }
        // Nested media, file paths and earlier partition nodes are not a
        // supported way to identify this initial physical-disk profile.
        if (type==4) return false;
        offset+=length;
    }
    return false;
}

// A candidate is the whole-disk path only when it is exactly the loaded
// path's prefix followed by End Entire; callers must reject ambiguity.
inline bool RecoveryParentPathMatches(const uint8_t* esp,uint32_t espBytes,
        const RecoveryEspPath& parsed,const uint8_t* parent,uint32_t parentBytes) {
    if(!esp || !parent || !parsed.parentPrefixBytes || espBytes>4096 || parentBytes<4 ||
       parsed.parentPrefixBytes>espBytes || parsed.parentPrefixBytes!=parentBytes-4) return false;
    for(uint32_t i=0;i<parsed.parentPrefixBytes;++i)if(esp[i]!=parent[i])return false;
    const uint32_t n=parsed.parentPrefixBytes;
    return parent[n]==0x7f && parent[n+1]==0xff && parent[n+2]==4 && parent[n+3]==0;
}
}} // namespace brook::bootloader
