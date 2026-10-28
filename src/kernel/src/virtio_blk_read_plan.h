#pragma once
#include <stdint.h>
namespace brook {
// Zero selects the existing parallel miss path. A nonzero result is one
// contiguous transfer bounded by the already allocated, request-guarded DMA
// buffer. Cache hits may leave holes; those must retain independent requests.
inline uint32_t VirtioContiguousReadBytes(const uint64_t* blocks,uint32_t count,
                                         uint32_t limit,uint32_t blockBytes,uint32_t dmaBytes) {
    if(!blocks || count<2 || count>limit || !blockBytes || count>dmaBytes/blockBytes)return 0;
    for(uint32_t i=1;i<count;++i)
        if(blocks[i]<blocks[0] || blocks[i]-blocks[0]!=i)return 0;
    return count*blockBytes;
}
}
