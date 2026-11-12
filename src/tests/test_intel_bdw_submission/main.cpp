#include "intel_bdw_submission.h"
#include <cassert>
using namespace brook;
int main(){
    BdwBcsSubmission s{{0x10000000,0x10002000,0x1234500000,16384,56,7},
        {{0x200000,256,64},{0x300000,256,64},16,4},0x100000,0x10008000,0x13579bdf};
    uint32_t ctx[2048]{},batch[12]{},ring[14]{};uint64_t desc=0;
    assert(BdwBuildBcsSubmission(s,ctx,batch,ring,desc));
    const uint32_t expectedBatch[]={0x54f00008,0x03cc0040,0,0x00040010,0x00300000,0,0,0x40,0x00200000,0,0x05000000,0};
    const uint32_t expectedRing[]={0x13244002,0xd0,0,0,0x04000000,0x18800101,0x00100000,0,0x13004002,0x10008004,0,0x13579bdf,0,0x04000001};
    for(unsigned i=0;i<12;++i)assert(batch[i]==expectedBatch[i]);
    for(unsigned i=0;i<14;++i)assert(ring[i]==expectedRing[i]);
    assert(desc==0x000000071000013dull && ctx[1024+7]==56);
    auto alias=s;assert(BdwBuildBcsSubmission(alias,ctx,batch,ring,alias.completionGgtt));
    assert(alias.completionGgtt==0x000000071000013dull);
    for(unsigned i=0;i<14;++i)assert(ring[i]==expectedRing[i]);
    auto adjacent=s;adjacent.completionGgtt=s.context.ringGgtt+s.context.ringBytes;
    assert(BdwBuildBcsSubmission(adjacent,ctx,batch,ring,desc));
    auto region=s;region.copy.widthPixels=5;region.copy.heightPixels=2;
    region.sourceX=3;region.sourceY=1;region.destinationX=7;region.destinationY=2;
    assert(BdwBuildBcsSubmission(region,ctx,batch,ring,desc));
    assert(batch[2]==0x00020007 && batch[3]==0x0004000c && batch[6]==0x00010003);
    for(unsigned i=0;i<14;++i)assert(ring[i]==expectedRing[i]);
    auto reject=[&](BdwBcsSubmission bad){for(auto& x:ctx)x=0xcccccccc;for(auto& x:batch)x=0xcccccccc;
        for(auto& x:ring)x=0xcccccccc;desc=0xfeed;assert(!BdwBuildBcsSubmission(bad,ctx,batch,ring,desc));
        for(auto x:ctx)assert(x==0xcccccccc);for(auto x:batch)assert(x==0xcccccccc);
        for(auto x:ring)assert(x==0xcccccccc);assert(desc==0xfeed);};
    auto b=s;b.completionGgtt+=32;reject(b);b=s;b.completionGgtt++;reject(b);
    b=s;b.completionGgtt=b.context.ringGgtt;reject(b);b=s;b.batchPpgtt=b.copy.source.gpuAddress;reject(b);
    b=s;b.batchPpgtt++;reject(b);b=s;b.batchPpgtt=uint64_t{1}<<32;reject(b);
    b=s;b.completionGgtt=b.context.contextGgtt+4096;reject(b);
    b=s;b.copy.source.gpuAddress=~uint64_t{0}-7;b.copy.source.allocationBytes=32;reject(b);
    b=s;b.context.contextGgtt=~uint64_t{0}-4095;reject(b);
    b=s;b.nonce=0;reject(b);b=s;b.context.tailBytes=48;reject(b);b=s;b.copy.widthPixels=0;reject(b);
    b=region;b.destinationY=3;reject(b);b=region;b.sourceX=12;reject(b);

    BdwBcsLaunch launch{s.context,s.batchPpgtt,s.completionGgtt,s.nonce};
    BdwBlitRegion regions[BdwBcsMaxRegions]{};
    for(unsigned i=0;i<BdwBcsMaxRegions;++i)
        regions[i]={{0x200000+i*64ull,64,64},{0x300000,2048,64},0,0,0,i,16,1};
    uint32_t packed[BdwBcsBatchWords]{};
    assert(BdwBuildBcsRegions(launch,regions,BdwBcsMaxRegions,ctx,packed,ring,desc));
    for(unsigned i=0;i<BdwBcsMaxRegions;++i) {
        assert(packed[i*10]==0x54f00008);
        assert(packed[i*10+2]==(i<<16));
        assert(packed[i*10+3]==(((i+1)<<16)|16));
        assert(packed[i*10+4]==0x300000);
        assert(packed[i*10+8]==0x200000+i*64);
    }
    assert(packed[BdwBcsBatchWords-2]==0x05000000 && packed[BdwBcsBatchWords-1]==0);
    for(unsigned i=0;i<14;++i)assert(ring[i]==expectedRing[i]);

    const auto rejectRegions=[&](const BdwBcsLaunch& request,unsigned count) {
        for(auto& word:ctx)word=0xcccccccc;
        for(auto& word:packed)word=0xcccccccc;
        for(auto& word:ring)word=0xcccccccc;
        desc=0xfeed;
        assert(!BdwBuildBcsRegions(request,regions,count,ctx,packed,ring,desc));
        for(auto word:ctx)assert(word==0xcccccccc);
        for(auto word:packed)assert(word==0xcccccccc);
        for(auto word:ring)assert(word==0xcccccccc);
        assert(desc==0xfeed);
    };
    rejectRegions(launch,0);
    rejectRegions(launch,BdwBcsMaxRegions+1);
    assert(!BdwBuildBcsRegions(launch,regions,2,ctx,batch,ring,desc));
    auto last=regions[BdwBcsMaxRegions-1];
    regions[BdwBcsMaxRegions-1].source.gpuAddress+=4;
    rejectRegions(launch,BdwBcsMaxRegions);
    regions[BdwBcsMaxRegions-1]=last;
    regions[BdwBcsMaxRegions-1].destination.gpuAddress+=64;
    rejectRegions(launch,BdwBcsMaxRegions);
    regions[BdwBcsMaxRegions-1]=last;
    auto overlappingBatch=launch;
    overlappingBatch.batchPpgtt=0x1fffc0;
    rejectRegions(overlappingBatch,BdwBcsMaxRegions);
}
