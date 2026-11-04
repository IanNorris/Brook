#pragma once
#include "intel_bdw_blitter.h"
#include "intel_bdw_context.h"
namespace brook {
// Constructs private RAM images only. The caller owns all GGTT/PPGTT mappings,
// must initialize the completion slot to a different value, publish CPU writes
// and page tables, and keep every object pinned until engine retirement.
struct BdwBcsSubmission {
    BdwContext64 context;
    BdwBlitCopy copy;
    uint64_t batchPpgtt=0,completionGgtt=0;
    uint32_t nonce=0;
};
inline bool BdwBuildBcsSubmission(const BdwBcsSubmission& s,uint32_t (&context)[2048],
                                  uint32_t (&batch)[12],uint32_t (&ring)[10],uint64_t& descriptor) {
    constexpr uint64_t limit=uint64_t{1}<<32;
    if(s.context.tailBytes!=40 || !s.nonce || (s.batchPpgtt&7) || s.batchPpgtt>limit-48 ||
       (s.completionGgtt&0x27) || s.completionGgtt>limit-8 || !s.completionGgtt)return false;
    uint32_t blit[10]{};if(!BdwEncodeLinearCopy(s.copy,blit))return false;
    const auto bounded=[&](uint64_t address,uint64_t size){return size && address<limit && size<=limit-address;};
    if(!bounded(s.context.contextGgtt,8192) || !bounded(s.context.ringGgtt,s.context.ringBytes))return false;
    const auto overlaps=[](uint64_t a,uint64_t size,uint64_t b,uint64_t other){return a<b+other && b<a+size;};
    // Batch is in PPGTT, the same address space as the copy operands.
    if(overlaps(s.batchPpgtt,48,s.copy.source.gpuAddress,s.copy.source.allocationBytes) ||
       overlaps(s.batchPpgtt,48,s.copy.destination.gpuAddress,s.copy.destination.allocationBytes))return false;
    // The GGTT completion write must never target either live command object.
    if(overlaps(s.completionGgtt,8,s.context.contextGgtt,8192) ||
       overlaps(s.completionGgtt,8,s.context.ringGgtt,s.context.ringBytes))return false;
    uint64_t builtDescriptor=0;
    if(!BdwBuildBcsContext64(s.context,context,builtDescriptor))return false;
    for(unsigned i=0;i<10;++i)batch[i]=blit[i];
    batch[10]=0x05000000;batch[11]=0;
    // The GGTT completion store must be in the privileged ring, not in the
    // nonprivileged batch; bit 5 of its address is prohibited on Gen8.
    const uint32_t commands[10]={0x04000000,0x18800101,uint32_t(s.batchPpgtt),uint32_t(s.batchPpgtt>>32),
        0x13004002,uint32_t(s.completionGgtt)|4,0,s.nonce,0,0x04000001};
    for(unsigned i=0;i<10;++i)ring[i]=commands[i];
    descriptor=builtDescriptor;return true;
}
}
