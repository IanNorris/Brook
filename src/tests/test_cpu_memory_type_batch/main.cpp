// The batched memory-type observation must equal the single-page observation
// (CpuObserveMemoryTypes(page,page)) for every page, and must fail closed on
// any configuration change inside its window.
#include "../../kernel/src/cpu_memory_type_batch.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"CHECK failed %s:%d %s\n",__FILE__,__LINE__,#x); std::abort(); } } while(0)
using namespace brook;
namespace {
FramebufferMtrrs config(){
    FramebufferMtrrs m{};m.capability=(1ull<<10)|3;m.count=3;m.physicalBits=39;m.defaults=(1ull<<11)|6;
    auto range=[&m](unsigned i,uint64_t base,uint64_t size,unsigned type){
        m.base[i]=base|type;m.mask[i]=((~(size-1))&((1ull<<m.physicalBits)-1)&~4095ull)|(1ull<<11);};
    range(0,0xc0000000ull,1ull<<30,0);   // MMIO hole UC
    range(1,0xe0000000ull,1ull<<28,1);   // overlapping WC inside UC -> UC
    range(2,0x100000000ull,1ull<<32,6);  // WB
    return m;
}
struct Hw {
    FramebufferMtrrs m=config();uint64_t cr0v=0x80050033,patv=0x0007040100070406ull;uint32_t cpuv=0;
    unsigned mtrrReads=0,changeMtrrAfter=~0u,changeCr0AfterReads=~0u,changePatAfterReads=~0u;unsigned cr0Reads=0,patReads=0;
    uint64_t cr0(){if(++cr0Reads>changeCr0AfterReads)return cr0v|(1ull<<30);return cr0v;}
    uint64_t pat(){if(++patReads>changePatAfterReads)return patv^1;return patv;}
    uint32_t cpu(){return cpuv;}
    bool mtrrs(FramebufferMtrrs& out){out=m;if(++mtrrReads>changeMtrrAfter)out.base[2]^=6^4;return true;}
};
// Reference: exactly CpuObserveMemoryTypes' algorithm for one page.
bool single(Hw& hw,uint64_t page,CpuMemoryTypeObservation& out){
    out={};FramebufferMtrrs before{},after{};const auto cr0=hw.cr0();
    if(!hw.mtrrs(before))return false;
    const uint64_t pat=hw.pat();const auto cpu=hw.cpu();
    if(!hw.mtrrs(after) || !FramebufferMtrrsEqual(before,after) || ((cr0^hw.cr0())&((1ull<<30)|(1ull<<29))) || hw.pat()!=pat)return false;
    CpuMemoryTypeObservation r{};r.cpu=cpu;r.patSlot0=uint8_t(pat&255);
    r.cacheDisabled=(cr0&(1ull<<30))!=0;r.notWriteThrough=(cr0&(1ull<<29))!=0;
    if(!MtrrObservePage(before,page,r.page0) || !MtrrObservePage(before,page,r.page1))return false;
    out=r;return true;
}
bool same(const CpuMemoryTypeObservation& a,const CpuMemoryTypeObservation& b){
    auto p=[](const MtrrPageObservation& x,const MtrrPageObservation& y){return x.type==y.type && x.matchedTypes==y.matchedTypes && x.usedDefault==y.usedDefault;};
    return a.cpu==b.cpu && a.patSlot0==b.patSlot0 && a.cacheDisabled==b.cacheDisabled && a.notWriteThrough==b.notWriteThrough && p(a.page0,b.page0) && p(a.page1,b.page1);
}
}
int main(){
    // The 13 pages the GPU critical-section audit checks, plus WB and default pages.
    const uint64_t pages[]={0xf600a000ull,0xf6130000ull,0xf6138000ull,0xf6022000ull,0xf6002000ull,0xf6012000ull,0xf601a000ull,
        0xf6000000ull,0xf6002000ull,0xf6100000ull,0xf600b000ull,0xf6880000ull,0xf6101000ull,0x180000000ull,0x80000000ull,0xe0001000ull};
    constexpr unsigned n=sizeof(pages)/8;
    {Hw hw;CpuMemoryTypeObservation batch[n]{};CHECK(CpuObserveMemoryTypesBatchWith(hw,pages,n,batch));
     CHECK(hw.mtrrReads==2);
     for(unsigned i=0;i<n;++i){Hw ref;CpuMemoryTypeObservation one{};CHECK(single(ref,pages[i],one));CHECK(same(one,batch[i]));}
     CHECK(batch[0].page0.type==0 && batch[13].page0.type==6 && batch[14].page0.usedDefault && batch[15].page0.type==0);}
    {Hw hw;hw.changeMtrrAfter=1;CpuMemoryTypeObservation b[n]{};CHECK(!CpuObserveMemoryTypesBatchWith(hw,pages,n,b));}
    {Hw hw;hw.changeCr0AfterReads=1;CpuMemoryTypeObservation b[n]{};CHECK(!CpuObserveMemoryTypesBatchWith(hw,pages,n,b));}
    {Hw hw;hw.changePatAfterReads=1;CpuMemoryTypeObservation b[n]{};CHECK(!CpuObserveMemoryTypesBatchWith(hw,pages,n,b));}
    {Hw hw;const uint64_t bad[]={0xf600a000ull,0xf600a001ull};CpuMemoryTypeObservation b[2]{};CHECK(!CpuObserveMemoryTypesBatchWith(hw,bad,2,b));
     Hw ref;CpuMemoryTypeObservation one{};CHECK(!single(ref,0xf600a001ull,one));}
    {Hw hw;hw.cpuv=2;CpuMemoryTypeObservation b[1]{};CHECK(CpuObserveMemoryTypesBatchWith(hw,pages,1,b) && b[0].cpu==2);}
    {Hw hw;CHECK(!CpuObserveMemoryTypesBatchWith(hw,pages,0,nullptr));}
    std::puts("CPU_MEMORY_TYPE_BATCH_PASS batch equals per-page observation, fails closed on MTRR/CR0/PAT change or bad page");
}
