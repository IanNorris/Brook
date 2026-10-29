#pragma once
#include <stdint.h>

namespace brook {
// Caller owns the device request guard for the whole operation. Cached() may
// return only a complete, valid 4 KiB block. No allocation or deferred writes.
template<class Backend>
bool VirtioCopyCachedSectors(Backend& io,uint64_t sector,uint32_t count,uint8_t* destination) {
    uint64_t remaining=uint64_t(count)*512,absolute=sector*512;
    while(remaining) {
        auto* cached=io.Cached(absolute/4096);
        if(!cached)return false; // caller's full preread replaces any prefix
        uint64_t offset=absolute%4096,bytes=4096-offset;
        if(bytes>remaining)bytes=remaining;
        __builtin_memcpy(destination,cached+offset,bytes);
        destination+=bytes;absolute+=bytes;remaining-=bytes;
    }
    return true;
}
template<class Backend>
void VirtioPatchCachedSectors(Backend& io,uint64_t sector,uint32_t count,const uint8_t* source) {
    uint64_t remaining=uint64_t(count)*512,absolute=sector*512;
    while(remaining) {
        uint64_t offset=absolute%4096,bytes=4096-offset;
        if(bytes>remaining)bytes=remaining;
        if(auto* cached=io.Cached(absolute/4096))__builtin_memcpy(cached+offset,source,bytes);
        source+=bytes;absolute+=bytes;remaining-=bytes;
    }
}
struct VirtioWriteResult { uint64_t bytes=0;bool ok=false; };
// Backend: Read/Write use this DMA buffer synchronously; ScatterWrite returns
// the complete byte count or failure; Invalidate takes a half-open sector span.
// A failed write may already have changed disk bytes, so invalidate its entire
// attempted range before any fallback or release of the caller's guard.
template<class Backend>
VirtioWriteResult VirtioPlanWrite(Backend& io,uint64_t offset,const uint8_t* source,uint64_t length,
                                  uint8_t* dma,uint32_t dmaSectors,uint32_t scatterSectors) {
    VirtioWriteResult result{};
    if(!length){result.ok=true;return result;}
    if(!source || !dma || !dmaSectors || length>~uint64_t{0}-offset)return result;
    const uint64_t first=offset/512,last=(offset+length)/512+((offset+length)%512!=0);
    const bool partialFirst=(offset%512)!=0,partialLast=((offset+length)%512)!=0;
    uint64_t sector=first;
    if(!partialFirst && !partialLast && scatterSectors) {
        while(sector<last) {
            uint32_t count=last-sector>scatterSectors?scatterSectors:static_cast<uint32_t>(last-sector);
            auto completed=io.ScatterWrite(sector,count,source+result.bytes);
            // Invalidate failures too: an attempted SG write may have reached
            // later sectors that a failing bounce retry never completes.
            io.Invalidate(sector,sector+count);
            if(completed!=static_cast<int64_t>(uint64_t(count)*512))break;
            sector+=count;result.bytes+=uint64_t(completed);
        }
    }
    while(sector<last && result.bytes<length) {
        uint32_t count=last-sector>dmaSectors?dmaSectors:static_cast<uint32_t>(last-sector);
        const bool preRead=(partialFirst && sector==first) ||
            (partialLast && sector+count==last) ||
            (last-first==1 && partialLast && sector==first);
        if(preRead && !VirtioCopyCachedSectors(io,sector,count,dma) && !io.Read(sector,count,dma))return result;
        uint64_t copied=result.bytes;
        for(uint32_t i=0;i<count && copied<length;++i) {
            const uint64_t begin=(sector+i)*512;
            const uint64_t skip=begin<offset?offset-begin:0;
            uint64_t bytes=512-skip;if(bytes>length-copied)bytes=length-copied;
            __builtin_memcpy(dma+uint64_t(i)*512+skip,source+copied,bytes);copied+=bytes;
        }
        if(!io.Write(sector,count,dma)){io.Invalidate(sector,sector+count);return result;}
        // Only update already-known whole blocks. Partial writes cannot
        // populate a missing block because its untouched bytes are unknown.
        VirtioPatchCachedSectors(io,sector,count,dma);
        result.bytes=copied;sector+=count;
    }
    result.ok=result.bytes==length;return result;
}
}
