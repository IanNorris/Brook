#include "intel_bdw_context.h"
#include "intel_bdw_ggtt_bind.h"
#include <cassert>
#include <cstdint>
using namespace brook;
struct FakeGgtt {
    uint64_t pte[16]{};unsigned writes=0;
    bool validateLease(const BdwGgttLease&){return true;}
    bool quiescent(){return true;}
    bool read(uint32_t index,uint64_t& out){assert(index>=65536 && index<65552);out=pte[index-65536];return true;}
    bool write(uint32_t index,uint64_t value){assert(index>=65536 && index<65552);pte[index-65536]=value;++writes;return true;}
    bool invalidate(){return true;}
};
int main(){
    BdwGgttLease lease{};lease.token=11;lease.count=11;
    for(unsigned i=0;i<lease.count;++i){lease.index[i]=65536+i;lease.dma[i]=0x200000+i*4096;}
    // The context and 16 KiB ring occupy adjacent, fully leased extents;
    // the page-table root is a distinct DMA address, never a GGTT address.
    BdwContext64 c{uint64_t(lease.index[0])*4096,uint64_t(lease.index[2])*4096,0x1234500000,16384,64,7};
    for(unsigned i=1;i<2;++i)assert(lease.index[i]==lease.index[0]+i);
    for(unsigned i=1;i<4;++i)assert(lease.index[2+i]==lease.index[2]+i);
    for(unsigned i=0;i<lease.count;++i)assert(c.pml4Dma!=lease.dma[i]);
    FakeGgtt io;BdwGgttBind binding;assert(binding.Bind(lease,io));
    uint32_t image[2048];uint64_t desc=0;
    assert(BdwBuildBcsContext64(c,image,desc) && desc==0x000000071000013dull);
    uint32_t expected[2048]{};auto* p=expected+1024;
    p[1]=0x11000015;const uint32_t a[]={0x22244,0x22034,0x22030,0x22038,0x2203c,0x22168,0x22140,0x22110,0x2211c,0x22114,0x22118};
    for(unsigned i=0;i<11;++i)p[2+2*i]=a[i];
    p[33]=0x11000011;const uint32_t b[]={0x223a8,0x2228c,0x22288,0x22284,0x22280,0x2227c,0x22278,0x22274,0x22270};
    for(unsigned i=0;i<9;++i)p[34+2*i]=b[i];
    p[65]=0x11000003;p[66]=0x22200;p[68]=0x22028;p[70]=0x05000000;
    p[3]=0x000f0009;p[7]=64;p[9]=0x10002000;p[11]=0x3001;p[49]=0x12;p[51]=0x34500000;
    for(unsigned i=0;i<2048;++i)assert(image[i]==expected[i]);
    auto reject=[&](BdwContext64 bad){for(auto& v:image)v=0xcccccccc;desc=0xfeed;
        assert(!BdwBuildBcsContext64(bad,image,desc) && desc==0xfeed);
        for(auto v:image)assert(v==0xcccccccc);};
    auto bad=c;bad.contextGgtt++;reject(bad);bad=c;bad.ringGgtt=c.contextGgtt+4096;reject(bad);
    bad=c;bad.contextGgtt=0xfffff000;reject(bad);bad=c;bad.ringGgtt=0xfffff000;reject(bad);
    bad=c;bad.ringBytes=12288;reject(bad);bad=c;bad.ringBytes=0;reject(bad);
    bad=c;bad.tailBytes=0;reject(bad);bad=c;bad.tailBytes=7;reject(bad);bad=c;bad.tailBytes=16384;reject(bad);
    bad=c;bad.pml4Dma=0;reject(bad);bad=c;bad.pml4Dma++;reject(bad);bad=c;bad.pml4Dma=uint64_t{1}<<39;reject(bad);
    bad=c;bad.contextId=0;reject(bad);bad=c;bad.contextId=0x200000;reject(bad);
    assert(binding.Retire(io) && binding.SafeToRelease() && io.writes==2*lease.count);
    for(unsigned i=0;i<lease.count;++i)assert(io.pte[i]==0);
}
