#pragma once
#include <stdint.h>
namespace brook {
// One permanently pinned GGTT scratch mapping. The caller must hold the
// device/forcewake lease, ensure this slot cannot be consumed by display or
// firmware, admit and pin the scratch page and serialize all page-table and
// engine access. The old value is never replayed. A failed store or drain may
// have reached hardware: after an attempted store, keep both slot and page
// pinned during the current ownership epoch, including on failure. GGTT PTEs
// may survive warm resets: the next owner must reset inherited execution and
// replace unused mappings before submitting work, while preserving display.
struct BdwScratchTakeoverResult {bool attempted=false,complete=false,resetAttempted=false,resetComplete=false;uint64_t previous=0;uint8_t stage=0;};
template<class Io> BdwScratchTakeoverResult BdwTakeoverScratchSlot(Io& io,uint32_t index,uint64_t dma,bool resetProven=false) {
    BdwScratchTakeoverResult r{};
    if(index<65536 || index>=1048576 || !dma || (dma&4095) || dma>=(uint64_t{1}<<39) ||
       (dma<0xdf800000ull && dma+4096>0xdd000000ull) || !io.admit(index,dma))return r;
    uint64_t first=0,second=0;
    // Reclaim a valid inherited entry only after the caller has reset the
    // relevant engine under the same lease and excluded display/other users.
    if(!io.read(index,first) || first==~uint64_t{0} || ((first&1) && !resetProven) ||
       !io.read(index,second) || first!=second || !io.admit(index,dma))return r;
    r.previous=first;r.attempted=true;
    const bool stored=io.write(index,dma|1);
    // Even a failed store may be posted; always attempt the drain.
    const bool drained=io.invalidate();
    if(!stored || !drained || !io.read(index,second) || second!=(dma|1) || !io.admit(index,dma))return r;
    r.complete=true;return r;
}
// Reset and takeover belong to one held owner/forcewake epoch. A failed reset
// never proceeds to a PTE write, even if the previous entry was invalid.
template<class Io,class Reset> BdwScratchTakeoverResult BdwResetAndTakeoverScratchSlot(Io& io,uint32_t index,uint64_t dma,Reset& reset) {
    BdwScratchTakeoverResult r{};
    if(index<65536 || index>=1048576 || !dma || (dma&4095) || dma>=(uint64_t{1}<<39) ||
       (dma<0xdf800000ull && dma+4096>0xdd000000ull) || !io.admit(index,dma))return r;
    r.resetAttempted=true;
    if(!reset())return r;
    r=BdwTakeoverScratchSlot(io,index,dma,true);
    r.resetAttempted=r.resetComplete=true;return r;
}
// Recovery of a mapping written by an earlier Brook boot. The caller must
// independently identify the exact stale PTE and exclude all consumers. A
// mismatch refuses all writes; an attempted clear is never retried blindly.
template<class Io> BdwScratchTakeoverResult BdwClearKnownScratchSlot(Io& io,uint32_t index,uint64_t expected) {
    BdwScratchTakeoverResult r{};
    if(index<65536 || index>=1048576 || !expected || !(expected&1) || expected==~uint64_t{0} || !io.admit(index,0))return r;
    uint64_t first=0,second=0;
    if(!io.read(index,first) || first!=expected || !io.read(index,second) || second!=expected || !io.admit(index,0))return r;
    r.previous=first;r.attempted=true;
    const bool stored=io.write(index,0);const bool drained=io.invalidate();
    if(!stored || !drained || !io.read(index,second) || second || !io.admit(index,0))return r;
    r.complete=true;return r;
}
}
