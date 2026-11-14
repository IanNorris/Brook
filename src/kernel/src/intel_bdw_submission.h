#pragma once
#include "intel_bdw_blitter.h"
#include "intel_bdw_context.h"
namespace brook {
constexpr uint32_t BdwBcsMaxRegions=32;
constexpr uint32_t BdwBcsBatchWords=BdwBcsMaxRegions*10+2;
// Persistent-target presents add up to 32 repair copies (front to back
// buffer) ahead of the renderer's 32 damage copies. The batch is one page.
constexpr uint32_t BdwBcsTargetMaxRegions=64;
constexpr uint32_t BdwBcsTargetBatchWords=BdwBcsTargetMaxRegions*10+2;
static_assert(BdwBcsTargetBatchWords*4<=4096);

// Constructs private RAM images only. The caller owns all GGTT/PPGTT mappings,
// must initialize the completion slot to a different value, publish CPU writes
// and page tables, and keep every object pinned until engine retirement.
struct BdwBcsLaunch {
    BdwContext64 context;
    uint64_t batchPpgtt=0,completionGgtt=0;
    uint32_t nonce=0;
};

// Each region writes to the same linear destination. A packed source can
// select a different base or pitch for each region.
template<unsigned BatchWords>
inline bool BdwBuildBcsRegions(const BdwBcsLaunch& s,const BdwBlitRegion* regions,uint32_t count,
                               uint32_t (&context)[2048],uint32_t (&batch)[BatchWords],
                               uint32_t (&ring)[14],uint64_t& descriptor) {
    static_assert(BatchWords>=12);
    constexpr uint32_t maxRegions=BatchWords>=BdwBcsTargetBatchWords?BdwBcsTargetMaxRegions:BdwBcsMaxRegions;
    if(!regions || !count || count>maxRegions || count>(BatchWords-2)/10)return false;
    constexpr uint64_t limit=uint64_t{1}<<32;
    const uint64_t batchBytes=(uint64_t(count)*10+2)*sizeof(uint32_t);
    if(s.context.tailBytes!=56 || !s.nonce || (s.batchPpgtt&7) ||
       s.batchPpgtt>limit-batchBytes ||
       (s.completionGgtt&0x27) || s.completionGgtt>limit-8 || !s.completionGgtt)return false;
    if(!BdwContext64Valid(s.context,8192))return false;
    const auto overlaps=[](uint64_t a,uint64_t size,uint64_t b,uint64_t other){return a<b+other && b<a+size;};
    // The GGTT completion write must never target either live command object.
    if(overlaps(s.completionGgtt,8,s.context.contextGgtt,8192) ||
       overlaps(s.completionGgtt,8,s.context.ringGgtt,s.context.ringBytes))return false;

    const auto& destination=regions[0].destination;
    constexpr unsigned capacity=(BatchWords-2)/10;
    uint32_t packets[capacity<maxRegions ? capacity : maxRegions][10];
    for(uint32_t i=0;i<count;++i) {
        const auto& region=regions[i];
        if(region.destination.gpuAddress!=destination.gpuAddress ||
           region.destination.allocationBytes!=destination.allocationBytes ||
           region.destination.pitchBytes!=destination.pitchBytes ||
           !BdwEncodeLinearRegion(region,packets[i]))return false;
        // The batch and the copy operands share the private address space.
        if(overlaps(s.batchPpgtt,batchBytes,region.source.gpuAddress,region.source.allocationBytes) ||
           overlaps(s.batchPpgtt,batchBytes,destination.gpuAddress,destination.allocationBytes))return false;
    }
    uint64_t builtDescriptor=0;
    if(!BdwBuildBcsContext64(s.context,context,builtDescriptor))return false;
    for(uint32_t i=0;i<count;++i)
        for(unsigned j=0;j<10;++j)batch[i*10+j]=packets[i][j];
    batch[count*10]=0x05000000;
    batch[count*10+1]=0;
    // The GGTT completion store must be in the privileged ring, not in the
    // nonprivileged batch; bit 5 of its address is prohibited on Gen8.
    // Gen8 XCS invalidates translation before fetching a batch in the new
    // PPGTT. STORE_INDEX addresses scratch in this context's HWSP, not GGTT.
    const uint32_t commands[14]={0x13244002,0xd0,0,0,0x04000000,
        0x18800101,uint32_t(s.batchPpgtt),uint32_t(s.batchPpgtt>>32),
        0x13004002,uint32_t(s.completionGgtt)|4,0,s.nonce,0,0x04000001};
    for(unsigned i=0;i<14;++i)ring[i]=commands[i];
    descriptor=builtDescriptor;
    return true;
}

struct BdwBcsSubmission {
    BdwContext64 context;
    BdwBlitCopy copy;
    uint64_t batchPpgtt=0,completionGgtt=0;
    uint32_t nonce=0;
    // Defaults preserve the original full-origin diagnostic copy.
    uint32_t sourceX=0,sourceY=0,destinationX=0,destinationY=0;
};
inline bool BdwBuildBcsSubmission(const BdwBcsSubmission& s,uint32_t (&context)[2048],
                                  uint32_t (&batch)[12],uint32_t (&ring)[14],uint64_t& descriptor) {
    const BdwBlitRegion region{s.copy.source,s.copy.destination,s.sourceX,s.sourceY,
        s.destinationX,s.destinationY,s.copy.widthPixels,s.copy.heightPixels};
    return BdwBuildBcsRegions({s.context,s.batchPpgtt,s.completionGgtt,s.nonce},&region,1,
                              context,batch,ring,descriptor);
}
}
