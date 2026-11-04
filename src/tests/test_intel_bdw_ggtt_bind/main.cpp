#include <cassert>
#include <type_traits>
#include "intel_bdw_ggtt_bind.h"
using namespace brook;
struct Fake {
    uint64_t pte[16]{0xdead0001,0xbeef0001};unsigned writes=0,invalidations=0,reads=0;
    unsigned failWrite=0,failInvalidate=0,badRead=0;bool valid=true,idle=true;
    BdwGgttBind* transaction=nullptr;
    bool validateLease(const BdwGgttLease&){return valid;}
    bool quiescent(){return idle;}
    bool read(uint32_t index,uint64_t& value){++reads;value=pte[index-65536];if(reads==badRead)value^=0x1000;return true;}
    bool write(uint32_t index,uint64_t value){if(transaction)assert(!transaction->SafeToRelease());++writes;pte[index-65536]=value;return writes!=failWrite;}
    bool invalidate(){return ++invalidations!=failInvalidate;}
};
struct DelayedFake : Fake {
    uint32_t indices[32]{};uint64_t values[32]{};unsigned pending=0;
    bool write(uint32_t index,uint64_t value){++writes;indices[pending]=index;values[pending++]=value;return writes!=failWrite;}
    bool invalidate(){if(++invalidations==failInvalidate)return false;
        for(unsigned i=0;i<pending;++i)pte[indices[i]-65536]=values[i];pending=0;return true;}
};
int main(){
    static_assert(!std::is_copy_constructible<BdwGgttBind>::value && !std::is_move_constructible<BdwGgttBind>::value);
    const BdwGgttLease lease{123,{65536,65537},{0x200000,0x201000},{0xdead0001,0xbeef0001}};
    {Fake f;BdwGgttBind b;f.transaction=&b;assert(b.Bind(lease,f) && b.State()==BdwGgttBindState::Bound && !b.SafeToRelease());
        assert(f.pte[0]==0x200001 && f.pte[1]==0x201001 && f.invalidations==1);
        assert(b.Retire(f) && b.SafeToRelease() && f.pte[0]==lease.expected[0] && f.pte[1]==lease.expected[1]);}
    BdwGgttLease full{};full.token=456;full.count=16;
    for(unsigned i=0;i<full.count;++i){full.index[i]=65536+i;full.dma[i]=0x300000+i*4096;full.expected[i]=i<2?lease.expected[i]:0;}
    {Fake f;BdwGgttBind b;assert(b.Bind(full,f));
        for(unsigned i=0;i<full.count;++i)assert(f.pte[i]==(full.dma[i]|1));
        assert(b.Retire(f) && b.SafeToRelease());for(unsigned i=0;i<full.count;++i)assert(f.pte[i]==full.expected[i]);}
    for(unsigned failure=1;failure<=full.count;++failure){DelayedFake f;f.failWrite=failure;BdwGgttBind b;
        assert(!b.Bind(full,f) && b.SafeToRelease() && !f.pending);
        for(unsigned i=0;i<full.count;++i)assert(f.pte[i]==full.expected[i]);}
    {Fake f;BdwGgttBind b;auto bad=full;bad.dma[15]=bad.dma[0];assert(!b.Bind(bad,f) && !f.reads && !f.writes);}
    {Fake f;BdwGgttBind b;auto bad=full;bad.index[15]=bad.index[0];assert(!b.Bind(bad,f) && !f.reads && !f.writes);}
    {Fake f;BdwGgttBind b;auto bad=full;bad.count=17;assert(!b.Bind(bad,f) && !f.reads && !f.writes);}
    {Fake f;BdwGgttBind b;auto bad=full;bad.count=0;assert(!b.Bind(bad,f) && !f.reads && !f.writes);}
    {Fake f;BdwGgttBind b;auto one=full;one.count=1;assert(b.Bind(one,f) && b.Retire(f) && b.SafeToRelease());}
    {Fake f;f.pte[1]=0;BdwGgttBind b;assert(!b.Bind(lease,f) && !f.writes && b.SafeToRelease());}
    {Fake f;f.valid=false;BdwGgttBind b;assert(!b.Bind(lease,f) && !f.reads && !f.writes);}
    for(unsigned failure=1;failure<=2;++failure){Fake f;f.failWrite=failure;BdwGgttBind b;
        assert(!b.Bind(lease,f) && b.SafeToRelease() && f.pte[0]==lease.expected[0] && f.pte[1]==lease.expected[1]);}
    {Fake f;f.failInvalidate=1;BdwGgttBind b;assert(!b.Bind(lease,f) && b.SafeToRelease() && f.invalidations==2);}
    {DelayedFake f;f.failWrite=2;BdwGgttBind b;assert(!b.Bind(lease,f) && b.SafeToRelease() && !f.pending && f.pte[0]==lease.expected[0] && f.pte[1]==lease.expected[1]);}
    {Fake f;f.badRead=3;BdwGgttBind b;assert(!b.Bind(lease,f) && b.SafeToRelease());}
    {Fake f;BdwGgttBind b;assert(b.Bind(lease,f));f.idle=false;assert(!b.Retire(f) && b.State()==BdwGgttBindState::Quarantined && !b.SafeToRelease());}
    {Fake f;BdwGgttBind b;assert(b.Bind(lease,f));f.failWrite=3;assert(!b.Retire(f) && b.State()==BdwGgttBindState::Quarantined && !b.SafeToRelease());}
    {Fake f;BdwGgttBind b;assert(b.Bind(lease,f));f.failInvalidate=2;assert(!b.Retire(f) && !b.SafeToRelease());}
    {Fake f;BdwGgttBind b;assert(b.Bind(lease,f));f.valid=false;const auto writes=f.writes;assert(!b.Retire(f) && f.writes==writes && !b.SafeToRelease());}
    {Fake f,other;BdwGgttBind b;assert(b.Bind(lease,f));assert(!b.Retire(other) && !other.writes && !b.SafeToRelease());}
    {Fake f;BdwGgttBind b;auto bad=lease;bad.dma[0]=0xdd800000;assert(!b.Bind(bad,f) && !f.reads && !f.writes);}
    {Fake f;BdwGgttBind b;auto bad=lease;bad.index[0]=65535;assert(!b.Bind(bad,f) && !f.reads && !f.writes);}
}
