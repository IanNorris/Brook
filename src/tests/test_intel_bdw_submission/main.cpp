#include "intel_bdw_submission.h"
#include <cassert>
using namespace brook;
int main(){
    BdwBcsSubmission s{{0x10000000,0x10002000,0x1234500000,16384,40,7},
        {{0x200000,256,64},{0x300000,256,64},16,4},0x100000,0x10008000,0x13579bdf};
    uint32_t ctx[2048]{},batch[12]{},ring[10]{};uint64_t desc=0;
    assert(BdwBuildBcsSubmission(s,ctx,batch,ring,desc));
    const uint32_t expectedBatch[]={0x54f00008,0x03cc0040,0,0x00040010,0x00300000,0,0,0x40,0x00200000,0,0x05000000,0};
    const uint32_t expectedRing[]={0x04000000,0x18800101,0x00100000,0,0x13004002,0x10008004,0,0x13579bdf,0,0x04000001};
    for(unsigned i=0;i<12;++i)assert(batch[i]==expectedBatch[i]);
    for(unsigned i=0;i<10;++i)assert(ring[i]==expectedRing[i]);
    assert(desc==0x000000071000013dull && ctx[1024+7]==40);
    auto alias=s;assert(BdwBuildBcsSubmission(alias,ctx,batch,ring,alias.completionGgtt));
    assert(alias.completionGgtt==0x000000071000013dull);
    for(unsigned i=0;i<10;++i)assert(ring[i]==expectedRing[i]);
    auto adjacent=s;adjacent.completionGgtt=s.context.ringGgtt+s.context.ringBytes;
    assert(BdwBuildBcsSubmission(adjacent,ctx,batch,ring,desc));
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
}
