#include "../../kernel/src/intel_bdw_render_plan.h"
#include "../../kernel/src/intel_bdw_render_ring.h"
#include <cassert>
using namespace brook;
static uint32_t context[20*1024];
static uint32_t saved[20*1024];
static uint32_t wa[1024];
struct Fake {
    uint64_t entries[32]{};unsigned writes=0,invalidations=0;bool idle=true;
    bool validateLease(const BdwGgttLease& lease){return lease.token==42;}
    bool quiescent(){return idle;}
    bool read(uint32_t index,uint64_t& value){value=entries[index-65536];return true;}
    bool write(uint32_t index,uint64_t value){entries[index-65536]=value;++writes;return true;}
    bool invalidate(){++invalidations;return true;}
};
int main() {
    uint32_t ring[56]{};
    assert(BdwBuildRenderRing(0x100000,0x200000,0x1234,false,false,ring));
    assert(ring[0]==0x7a000004 && ring[38]==0x04000000 && ring[39]==0x18800001);
    assert(ring[40]==0x100000 && ring[41]==0 && ring[50]==0x200000 && ring[52]==0x1234);
    assert(BdwBuildRenderRing(0x123456780008ull,0x200000,1,true,false,ring));
    assert(ring[39]==0x18800101 && ring[40]==0x56780008 && ring[41]==0x1234);
    const auto savedWord=ring[39];
    assert(!BdwBuildRenderRing(0x100001,0x200000,1,true,false,ring) && ring[39]==savedWord);
    assert(!BdwBuildRenderRing(uint64_t{1}<<48,0x200000,1,true,false,ring));
    assert(!BdwBuildRenderRing(uint64_t{1}<<32,0x200000,1,false,false,ring));
    assert(!BdwBuildRenderRing(0x100000,0x200020,1,true,false,ring));
    assert(!BdwBuildRenderRing(0x100000,0x200000,0,true,false,ring));
    uint64_t pages[BdwRenderPlan::PageCount],expected[BdwRenderPlan::PageCount]{};
    for(unsigned i=0;i<BdwRenderPlan::PageCount;++i)pages[i]=0x400000+i*4096;
    BdwRenderPlan plan{};
    assert(BdwBuildRenderPlan(65536,42,3,56,0x900000,pages,expected,context,wa,plan));
    assert(plan.lease.count==28 && plan.lease.index[19]==65555 && plan.lease.index[20]==65556);
    assert(plan.hwsGgtt==(65536ull+24)*4096 && plan.completionGgtt==(65536ull+25)*4096);
    assert(plan.waGgtt==(65536ull+26)*4096 && plan.waScratchGgtt==(65536ull+27)*4096);
    assert(context[1024+9]==(65536+20)*4096 && context[1024+51]==0x900000);
    assert(context[1024+27]==(plan.waGgtt|2) && wa[3]==plan.waScratchGgtt+256);
    saved[4096]=0x1234abcd;saved[1024+25]=0xffffffffu;
    BdwRenderPlan rebased{};
    assert(BdwBuildRenderPlan(65565,43,4,224,0x901000,pages,expected,context,wa,rebased,&saved));
    assert(context[4096]==0x1234abcdu && context[1024+3]==0x000f0008 && context[1024+25]==0);
    assert(context[1024+9]==(65565u+20)*4096 && context[1024+51]==0x901000);
    assert(context[1024+27]==(rebased.waGgtt|2) && rebased.descriptor==((uint64_t{4}<<32)|(65565ull<<12)|0x13d));
    assert(BdwBuildRenderPlan(65536,42,3,56,0x900000,pages,expected,context,wa,plan));
    Fake io;BdwGgttBind bind;assert(bind.Bind(plan.lease,io) && !bind.SafeToRelease());
    for(unsigned i=0;i<28;++i)assert(io.entries[i]==(pages[i]|1));
    assert(bind.Retire(io) && bind.SafeToRelease());
    for(unsigned i=0;i<28;++i)assert(io.entries[i]==0);
    for(auto& word:context)word=0x5a5a5a5a;
    auto old=plan;pages[25]=pages[0];
    assert(!BdwBuildRenderPlan(65536,42,3,56,0x900000,pages,expected,context,wa,plan));
    assert(plan.descriptor==old.descriptor && plan.lease.token==old.lease.token);
    for(auto word:context)assert(word==0x5a5a5a5a);
    pages[25]=0x419000;
    assert(!BdwBuildRenderPlan(1048576-27,42,3,56,0x900000,pages,expected,context,wa,plan));
    assert(!BdwBuildRenderPlan(65536,42,3,56,pages[0],pages,expected,context,wa,plan));
    assert(!BdwBuildRenderPlan(65536,42,3,56,0xdd000000,pages,expected,context,wa,plan));
    for(auto word:context)assert(word==0x5a5a5a5a);
}
