#pragma once
#include <stdint.h>
#include "intel_bdw_oneshot.h"

namespace brook {
// Match the variable PPGTT allocation limit so the complete table image can
// be validated before its first cache flush.
constexpr uint32_t BdwPublishMaximumPages=128;
// CPU cache publication only. Caller owns every complete page, excludes CPU
// writers, retains pages until the final barrier, and must separately prove
// mapping types, DMA translation and device-side visibility before submission.
// All ranges and the line size are checked before issuing the first flush.
template<class Flush>
bool BdwPublishCpuPages(Flush& flush,const BdwOwnedPage* pages,unsigned count,uint32_t line) {
    if(!pages || !count || count>BdwPublishMaximumPages || line<16 || line>4096 ||
       (line&(line-1)) || (4096%line))return false;
    for(unsigned i=0;i<count;++i) {
        const uintptr_t p=reinterpret_cast<uintptr_t>(pages[i].cpu);
        if(!p || (p&4095) || p>~uintptr_t(0)-4096)return false;
        for(unsigned j=0;j<i;++j)if(pages[j].cpu==pages[i].cpu)return false;
    }
    flush.fence();
    for(unsigned i=0;i<count;++i)
        for(unsigned offset=0;offset<4096;offset+=line)flush.line(pages[i].cpu+offset);
    flush.fence();
    return true;
}
// Publication of larger private sets (PPGTT tables for big jobs: up to 512
// tables plus 4 scratch pages, more than one BdwPublishCpuPages call takes).
// The whole set is validated (non-null, aligned, no duplicate pages) before
// the first flush, as for a single call; flushing then proceeds in chunks of
// BdwPublishBatchPages, each passed to `chunk`, so a caller can bound the time
// it runs with interrupts disabled per chunk instead of per set.
constexpr uint32_t BdwPublishBatchPages=64;
constexpr uint32_t BdwPublishMaximumBatchedPages=1024;
template<class Chunk>
bool BdwPublishCpuPagesBatched(const BdwOwnedPage* pages,unsigned count,Chunk chunk) {
    if(!pages || !count || count>BdwPublishMaximumBatchedPages)return false;
    for(unsigned i=0;i<count;++i) {
        const uintptr_t p=reinterpret_cast<uintptr_t>(pages[i].cpu);
        if(!p || (p&4095) || p>~uintptr_t(0)-4096)return false;
        for(unsigned j=0;j<i;++j)if(pages[j].cpu==pages[i].cpu)return false;
    }
    for(unsigned first=0;first<count;first+=BdwPublishBatchPages){
        const unsigned n=count-first<BdwPublishBatchPages?count-first:BdwPublishBatchPages;
        if(!chunk(pages+first,n))return false;
    }
    return true;
}
}
