#pragma once
#include <stdint.h>
namespace brook {
// QEMU string fw_cfg entries may have one trailing NUL. Reject malformed or
// truncated values instead of silently selecting a larger buffering policy.
inline bool AudioParseStagingFrames(const char* text,uint32_t size,uint32_t* frames) {
    if(!text || !frames || !size)return false;
    if(!text[size-1])--size;
    if(!size || size>4)return false;
    uint32_t value=0;
    for(uint32_t i=0;i<size;++i) {
        if(text[i]<'0' || text[i]>'9')return false;
        value=value*10+uint32_t(text[i]-'0');
    }
    if(value>4096)return false;
    *frames=value;return true;
}
// A validated native copy owns its samples independently of the client FIFO.
// Staging is bounded outstanding source, not fictitious playback completion.
inline bool AudioQueueAvailable(uint32_t capacity,uint64_t software,uint32_t inFlight,
                                uint32_t staging,uint32_t* available) {
    if(!available)return false;
    *available=0;
    if(!capacity || staging>4096 || software>capacity)return false;
    const uint64_t charged=software+(inFlight>staging?inFlight-staging:0);
    if(charged>capacity || software+inFlight>uint64_t(capacity)+staging)return false;
    *available=capacity-static_cast<uint32_t>(charged);return true;
}
}
