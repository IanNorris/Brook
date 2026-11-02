#pragma once
#include <stdint.h>

namespace brook {
// Flush a byte range in a page-aligned, page-sized write-back DMA allocation.
// A flush publishes entire cache lines, including bytes adjacent to the range;
// the caller must own those lines and serialize all CPU writers. It must also
// prove that the device cannot consume the interval while it is being written
// or flushed. This primitive does not establish either property or issue the
// required final fence. Validation completes before any line is flushed.
template<class Flush>
bool HdaDmaFlushRange(Flush& flush, void* buffer, uint32_t size,
                      uint32_t offset, uint32_t bytes, uint32_t line) {
    const auto base=reinterpret_cast<uintptr_t>(buffer);
    if(!base || (base&4095) || !size || (size&4095) ||
       base>~uintptr_t(0)-size || line<16 || line>4096 ||
       (line&(line-1)) || (4096%line) || offset>size || bytes>size-offset)
        return false;
    if(!bytes)return true;
    const uint32_t first=offset&~(line-1);
    const uint32_t last=(offset+bytes-1)&~(line-1);
    for(uint32_t current=first;;current+=line) {
        flush.line(reinterpret_cast<void*>(base+current));
        if(current==last)break;
    }
    return true;
}
}
