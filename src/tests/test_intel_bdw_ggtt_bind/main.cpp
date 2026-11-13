#include <cassert>
#include <type_traits>
#include <vector>
#include "intel_bdw_ggtt_bind.h"
#include "intel_bdw_ggtt_extent_bind.h"
#include "intel_bdw_ggtt_extent_mmio.h"
#include "intel_bdw_ggtt_mmio.h"
#include "intel_bdw_scratch_takeover.h"
#include "intel_bdw_scratch_mmio.h"
using namespace brook;
struct ExtentFake {
    std::vector<uint64_t> pte=std::vector<uint64_t>(5625);
    std::vector<std::pair<uint32_t,uint64_t>> posted;
    unsigned writes=0,invalidations=0,failWrite=0,failInvalidate=0;bool valid=true,idle=true,delay=false;
    BdwGgttExtentBind* transaction=nullptr;
    bool validateLease(const BdwGgttExtentLease& l){return valid && l.reservation.token==777 && l.reservation.count==pte.size();}
    bool validateTakeover(const BdwGgttExtentLease& l,uint64_t scratch){return validateLease(l) && scratch==0x400000;}
    bool quiescent(){return idle;}
    bool read(uint32_t i,uint64_t& value){if(i<65536 || i-65536>=pte.size())return false;value=pte[i-65536];return true;}
    bool write(uint32_t i,uint64_t value){assert(transaction && !transaction->SafeToRelease());++writes;
        if(delay)posted.push_back({i-65536,value});else pte[i-65536]=value;return writes!=failWrite;}
    bool invalidate(){if(++invalidations==failInvalidate)return false;
        for(auto p:posted)pte[p.first]=p.second;
        posted.clear();return true;}
};
struct ExtentMmioOwner {
    volatile uint64_t* expected[2]{};bool held=true,idle=true,audit=true,takeover=false;unsigned invalidations=0;
    bool readable(){return held;} bool owned(){return held;} bool quiescent(){return idle;}
    bool validateLease(const BdwGgttExtentLease& l){return l.reservation.token==778;}
    bool validateTakeover(const BdwGgttExtentLease&,uint64_t scratch){return held && takeover && scratch==0x400000;}
    bool auditPtePage(uint32_t page,volatile uint64_t* p){return audit && page>=128 && page<=129 && p==expected[page-128];}
    bool invalidateGpuTranslations(){++invalidations;return true;}
};
struct LargeMmioOwner {
    volatile uint64_t* const* expected;unsigned invalidations=0;
    bool readable(){return true;} bool owned(){return true;} bool quiescent(){return true;}
    bool validateLease(const BdwGgttExtentLease& l){return l.reservation.token==779;}
    bool validateTakeover(const BdwGgttExtentLease&,uint64_t){return false;}
    bool auditPtePage(uint32_t page,volatile uint64_t* p){return page>=128 && page<139 && p==expected[page-128];}
    bool invalidateGpuTranslations(){++invalidations;return true;}
};
struct Fake {
    uint64_t pte[32]{0xdead0001,0xbeef0001};unsigned writes=0,invalidations=0,reads=0;
    unsigned failWrite=0,failInvalidate=0,badRead=0;bool valid=true,idle=true,takeover=true;
    BdwGgttBind* transaction=nullptr;
    bool validateLease(const BdwGgttLease&){return valid;}
    bool validateTakeover(const BdwGgttLease&,uint64_t){return takeover;}
    bool quiescent(){return idle;}
    bool read(uint32_t index,uint64_t& value){++reads;value=pte[index-65536];if(reads==badRead)value^=0x1000;return true;}
    bool write(uint32_t index,uint64_t value){if(transaction)assert(!transaction->SafeToRelease());++writes;pte[index-65536]=value;return writes!=failWrite;}
    bool invalidate(){return ++invalidations!=failInvalidate;}
};
struct DelayedFake : Fake {
    uint32_t indices[64]{};uint64_t values[64]{};unsigned pending=0;
    bool write(uint32_t index,uint64_t value){++writes;indices[pending]=index;values[pending++]=value;return writes!=failWrite;}
    bool invalidate(){if(++invalidations==failInvalidate)return false;
        for(unsigned i=0;i<pending;++i)pte[indices[i]-65536]=values[i];pending=0;return true;}
};
struct MmioOwner {
    bool held=true,idle=true,writeHeld=true,invalidateOk=true;unsigned invalidations=0;
    bool readable(){return held;}
    bool owned(){return held && writeHeld;}
    bool quiescent(){return idle;}
    bool invalidateGpuTranslations(){++invalidations;return invalidateOk;}
    bool validateLease(const BdwGgttLease& l){return owned() && l.token==123;}
    bool validateTakeover(const BdwGgttLease&,uint64_t){return owned();}
};
struct ScratchIo {
    uint64_t value=0xba141f3d556fc1caull;unsigned reads=0,writes=0,drains=0;
    bool allowed=true,change=false,failWrite=false,failDrain=false;
    bool admit(uint32_t i,uint64_t p){return allowed && i==65536 && p==0x400000;}
    bool read(uint32_t,uint64_t& v){v=value;if(change && ++reads==2)v^=1;return true;}
    bool write(uint32_t,uint64_t v){++writes;value=v;return !failWrite;}
    bool invalidate(){++drains;return !failDrain;}
};
int main(){
    {
        alignas(4096) static uint64_t physical[2][512]{};uint32_t flush=0;
        volatile uint64_t* mapped[]={physical[0],physical[1]};ExtentMmioOwner owner{{mapped[0],mapped[1]}};
        uint64_t dma[]={0x100000,0x101000,0x102000,0x103000,0x104000,0x105000};
        BdwGgttExtentLease l{{65536+509,6,778},dma};
        BdwGgttExtentMmio<ExtentMmioOwner> io(owner,l,mapped,2,&flush);
        assert(io.validateLease(l));uint64_t value=0;
        assert(!io.read(65536+508,value) && !io.write(65536+515,0x106001));
        assert(!io.write(65536+509,0x999001));
        BdwGgttExtentBind bind;assert(bind.Bind(l,io));
        for(unsigned i=0;i<6;++i)assert(physical[(509+i)/512][(509+i)%512]==(dma[i]|1));
        assert(flush==1 && owner.invalidations==1 && bind.Retire(io));
        for(unsigned i=0;i<6;++i)assert(!physical[(509+i)/512][(509+i)%512]);
        owner.audit=false;assert(!io.validateLease(l) && !io.read(65536+509,value));
        owner.audit=true;owner.held=false;assert(!io.write(65536+509,dma[0]|1) && !io.invalidate());
    }
    {
        alignas(4096) static uint64_t physical[11][512]{};volatile uint64_t* mapped[11]{};
        for(unsigned i=0;i<11;++i)mapped[i]=physical[i];
        LargeMmioOwner owner{mapped};uint32_t flush=0;
        std::vector<uint64_t> dma(5625);for(uint32_t i=0;i<dma.size();++i)dma[i]=0x100000000ull+uint64_t(i)*4096;
        BdwGgttExtentLease l{{65536,uint32_t(dma.size()),779},dma.data()};
        BdwGgttExtentMmio<LargeMmioOwner> io(owner,l,mapped,11,&flush);BdwGgttExtentBind bind;
        assert(bind.Bind(l,io) && physical[0][0]==(dma[0]|1) && physical[10][504]==(dma[5624]|1));
        assert(bind.Retire(io) && !physical[0][0] && !physical[10][504] && bind.SafeToRelease());
    }
    {
        alignas(4096) static uint64_t physical[2][512]{};uint32_t flush=0;
        volatile uint64_t* mapped[]={physical[0],physical[1]};ExtentMmioOwner owner{{mapped[0],mapped[1]}};
        uint64_t dma[]={0x100000,0x101000},expected[]={0xdead0001,0xbeef0000};
        physical[0][510]=expected[0];physical[0][511]=expected[1];
        BdwGgttExtentLease l{{65536+510,2,778},dma};
        BdwGgttExtentMmio<ExtentMmioOwner> io(owner,l,mapped,1,&flush,0x400000);
        BdwGgttExtentBind b;
        assert(!b.BindTakenOver(l,io,expected,0x400000));
        owner.takeover=true;assert(b.BindTakenOver(l,io,expected,0x400000));
        assert(b.Retire(io) && physical[0][510]==0x400001 && physical[0][511]==0x400001);
    }
    {
        alignas(4096) static uint64_t physical[512]{};uint32_t flush=0;
        volatile uint64_t* mapped[]={physical};ExtentMmioOwner owner{{mapped[0],nullptr}};owner.takeover=true;
        uint64_t dma[]={0x100000,0x101000},expected[]={0xdead0001,0xbeef0000};
        physical[0]=expected[0];physical[1]=expected[1];
        BdwGgttExtentLease l{{65536,2,778},dma};
        BdwGgttExtentMmio<ExtentMmioOwner> io(owner,l,mapped,1,&flush,0x400000);
        BdwGgttExtentBind b;assert(b.BindTakenOver(l,io,expected,0x400000));
        assert(b.RetireToInvalid(io) && b.SafeToRelease() && !physical[0] && !physical[1]);
        assert(owner.invalidations==3);
        assert(b.Recycle() && b.Bind(l,io));assert(!b.Recycle());assert(b.Retire(io) && b.Recycle());
    }
    {
        std::vector<uint64_t> pages(5625);for(uint32_t i=0;i<pages.size();++i)pages[i]=0x100000000ull+uint64_t(i)*4096;
        BdwGgttExtentLease l{{65536,uint32_t(pages.size()),777},pages.data()};
        {ExtentFake f;BdwGgttExtentBind b;f.transaction=&b;f.pte[513]=1;
         assert(!b.Bind(l,f) && !f.writes && b.SafeToRelease());}
        {ExtentFake f;BdwGgttExtentBind b;f.transaction=&b;f.delay=true;
         assert(b.Bind(l,f) && b.State()==BdwGgttExtentState::Bound && !b.SafeToRelease());
         assert(f.pte[0]==(pages[0]|1) && f.pte[5624]==(pages[5624]|1));
         assert(b.Retire(f) && b.SafeToRelease());for(auto p:f.pte)assert(!p);}
        {ExtentFake f;BdwGgttExtentBind b;f.transaction=&b;f.delay=true;f.failWrite=513;
         assert(!b.Bind(l,f) && b.State()==BdwGgttExtentState::Restored && b.SafeToRelease());
         for(auto p:f.pte)assert(!p);}
        {ExtentFake f;BdwGgttExtentBind b;f.transaction=&b;f.failWrite=513;f.failInvalidate=1;
         assert(!b.Bind(l,f) && b.State()==BdwGgttExtentState::Quarantined && !b.SafeToRelease());}
        {ExtentFake f;BdwGgttExtentBind b;f.transaction=&b;f.failInvalidate=1;
         assert(!b.Bind(l,f) && f.writes==5625 && !b.SafeToRelease());}
        {ExtentFake f;BdwGgttExtentBind b;f.transaction=&b;assert(b.Bind(l,f));f.pte[513]=0x12345001;
         const auto writes=f.writes;assert(!b.Retire(f) && !b.SafeToRelease() && f.writes==writes);}
        {ExtentFake f;BdwGgttExtentBind b;f.transaction=&b;assert(b.Bind(l,f));f.idle=false;
         assert(!b.Retire(f) && !b.SafeToRelease());}
        {ExtentFake f,other;BdwGgttExtentBind b;f.transaction=&b;other.transaction=&b;assert(b.Bind(l,f));
         assert(!b.Retire(other) && !b.SafeToRelease() && !other.writes);}
        {ExtentFake f;BdwGgttExtentBind b;f.transaction=&b;pages[513]=0xdd000000ull;
         assert(!b.Bind(l,f) && !f.writes && b.SafeToRelease());pages[513]=0x100000000ull+uint64_t(513)*4096;}
        {ExtentFake f;BdwGgttExtentBind b;f.transaction=&b;std::vector<uint64_t> before(5625,0xdead0001);
         f.pte=before;f.delay=true;f.failWrite=513;
         assert(!b.BindTakenOver(l,f,before.data(),0x400000) && b.SafeToRelease());
         for(unsigned i=0;i<513;++i)assert(f.pte[i]==0x400001);
         assert(f.pte[513]==before[513]);}
        {ExtentFake f;BdwGgttExtentBind b;f.transaction=&b;std::vector<uint64_t> before(5625,0xdead0001);
         f.pte=before;f.pte[12]^=2;assert(!b.BindTakenOver(l,f,before.data(),0x400000) && !f.writes);}
        {ExtentFake f;BdwGgttExtentBind b;f.transaction=&b;std::vector<uint64_t> before(5625,0xdead0001);
         f.pte=before;f.delay=true;f.failWrite=513;f.failInvalidate=1;
         assert(!b.BindTakenOver(l,f,before.data(),0x400000) && !b.SafeToRelease() && b.State()==BdwGgttExtentState::Quarantined);}
        {ExtentFake f;BdwGgttExtentBind b;f.transaction=&b;std::vector<uint64_t> before(5625,0xdead0001);
         f.pte=before;assert(b.BindTakenOver(l,f,before.data(),0x400000));f.pte[17]^=0x1000;
         const auto writes=f.writes;assert(!b.Retire(f) && !b.SafeToRelease() && f.writes==writes);}
        {ExtentFake f;BdwGgttExtentBind b;f.transaction=&b;std::vector<uint64_t> before(5625,0xdead0001);
         f.pte=before;f.delay=true;assert(b.BindTakenOver(l,f,before.data(),0x400000));
         assert(b.RetireToInvalid(f) && b.SafeToRelease() && f.posted.empty());for(auto p:f.pte)assert(!p);}
        {ExtentFake f;BdwGgttExtentBind b;f.transaction=&b;std::vector<uint64_t> before(5625,0xdead0001);
         f.pte=before;assert(b.BindTakenOver(l,f,before.data(),0x400000));f.failInvalidate=f.invalidations+2;
         assert(!b.RetireToInvalid(f) && !b.SafeToRelease() && b.State()==BdwGgttExtentState::Quarantined);}
    }
    {MmioOwner owner;uint64_t pte=119971841;uint32_t flush=0;
     BdwScratchMmio<MmioOwner> io(owner,&pte,&flush,65536,0);
     auto r=BdwClearKnownScratchSlot(io,65536,119971841);
     assert(r.attempted && r.complete && pte==0 && flush==1 && owner.invalidations==1);}
    {MmioOwner owner;uint64_t pte=119971843;uint32_t flush=0;
     BdwScratchMmio<MmioOwner> io(owner,&pte,&flush,65536,0);
     auto r=BdwClearKnownScratchSlot(io,65536,119971841);
     assert(!r.attempted && pte==119971843 && !flush);}
    {MmioOwner owner;uint64_t pte=0xba141f3d556fc1caull;uint32_t flush=0;
     BdwScratchMmio<MmioOwner> io(owner,&pte,&flush,65536,0x400000);
     auto r=BdwTakeoverScratchSlot(io,65536,0x400000);
     assert(r.attempted && r.complete && pte==0x400001 && flush==1 && owner.invalidations==1);
     assert(!io.write(65536,0x500001) && !io.write(65537,0x400001));}
    {MmioOwner owner;owner.invalidateOk=false;uint64_t pte=0;uint32_t flush=0;
     BdwScratchMmio<MmioOwner> io(owner,&pte,&flush,65536,0x400000);
     auto r=BdwTakeoverScratchSlot(io,65536,0x400000);
     assert(r.attempted && !r.complete && pte==0x400001 && owner.invalidations==1);}
    {ScratchIo f;auto r=BdwTakeoverScratchSlot(f,65536,0x400000);
     assert(r.attempted && r.complete && r.previous==0xba141f3d556fc1caull && f.value==0x400001 && f.drains==1);}
    {ScratchIo f;f.change=true;auto r=BdwTakeoverScratchSlot(f,65536,0x400000);
     assert(!r.attempted && !f.writes && !f.drains);}
    {ScratchIo f;f.value=0x100001;auto r=BdwTakeoverScratchSlot(f,65536,0x400000);
     assert(!r.attempted && !f.writes && !f.drains);}
    {ScratchIo f;f.value=0x100001;auto r=BdwTakeoverScratchSlot(f,65536,0x400000,true);
     assert(r.attempted && r.complete && r.previous==0x100001 && f.value==0x400001 && f.drains==1);}
    {ScratchIo f;f.value=0x100001;auto fail=[](){return false;};
     auto r=BdwResetAndTakeoverScratchSlot(f,65536,0x400000,fail);
     assert(r.resetAttempted && !r.resetComplete && !r.attempted && !f.writes && !f.drains);}
    {ScratchIo f;f.value=0x100001;auto succeed=[](){return true;};
     auto r=BdwResetAndTakeoverScratchSlot(f,65536,0x400000,succeed);
     assert(r.resetAttempted && r.resetComplete && r.complete && f.value==0x400001);}
    {ScratchIo f;f.failWrite=true;auto r=BdwTakeoverScratchSlot(f,65536,0x400000);
     assert(r.attempted && !r.complete && f.writes==1 && f.drains==1);}
    {ScratchIo f;f.failDrain=true;auto r=BdwTakeoverScratchSlot(f,65536,0x400000);
     assert(r.attempted && !r.complete && f.writes==1 && f.drains==1);}
    static_assert(!std::is_copy_constructible<BdwGgttBind>::value && !std::is_move_constructible<BdwGgttBind>::value);
    const BdwGgttLease lease{123,{65536,65537},{0x200000,0x201000},{0xdead0001,0xbeef0001}};
    {Fake f;BdwGgttBind b;f.transaction=&b;assert(b.Bind(lease,f) && b.State()==BdwGgttBindState::Bound && !b.SafeToRelease());
        assert(f.pte[0]==0x200001 && f.pte[1]==0x201001 && f.invalidations==1);
        assert(b.Retire(f) && b.SafeToRelease() && f.pte[0]==lease.expected[0] && f.pte[1]==lease.expected[1]);}
    BdwGgttLease full{};full.token=456;full.count=32;
    for(unsigned i=0;i<full.count;++i){full.index[i]=65536+i;full.dma[i]=0x300000+i*4096;full.expected[i]=i<2?lease.expected[i]:0;}
    {Fake f;BdwGgttBind b;assert(b.Bind(full,f));
        for(unsigned i=0;i<full.count;++i)assert(f.pte[i]==(full.dma[i]|1));
        assert(b.Retire(f) && b.SafeToRelease());for(unsigned i=0;i<full.count;++i)assert(f.pte[i]==full.expected[i]);}
    for(unsigned failure=1;failure<=full.count;++failure){DelayedFake f;f.failWrite=failure;BdwGgttBind b;
        assert(!b.Bind(full,f) && b.SafeToRelease() && !f.pending);
        for(unsigned i=0;i<full.count;++i)assert(f.pte[i]==full.expected[i]);}
    {Fake f;BdwGgttBind b;auto bad=full;bad.dma[31]=bad.dma[0];assert(!b.Bind(bad,f) && !f.reads && !f.writes);}
    {Fake f;BdwGgttBind b;auto bad=full;bad.index[31]=bad.index[0];assert(!b.Bind(bad,f) && !f.reads && !f.writes);}
    {Fake f;BdwGgttBind b;auto bad=full;bad.count=33;assert(!b.Bind(bad,f) && !f.reads && !f.writes);}
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
    {MmioOwner owner;uint64_t ptes[512]{};uint32_t flush=0;
        ptes[0]=lease.expected[0];ptes[1]=lease.expected[1];
        BdwGgttMmio<MmioOwner> io(owner,ptes,&flush,65536,2);BdwGgttBind b;
        assert(b.Bind(lease,io) && ptes[0]==0x200001 && ptes[1]==0x201001 && flush==1);
        assert(b.Retire(io) && ptes[0]==lease.expected[0] && ptes[1]==lease.expected[1]);
        assert(owner.invalidations==2);
        uint64_t out=0;assert(!io.read(65535,out) && !io.write(65538,1));
        owner.writeHeld=false;assert(io.read(65536,out) && !io.write(65536,1) && !io.invalidate());
        owner.held=false;flush=0;assert(!io.read(65536,out) && !io.write(65536,1) && !io.invalidate() && flush==0);}
    {MmioOwner owner;owner.invalidateOk=false;uint64_t ptes[512]{};uint32_t flush=0;
        ptes[0]=lease.expected[0];ptes[1]=lease.expected[1];BdwGgttMmio<MmioOwner> io(owner,ptes,&flush,65536,2);BdwGgttBind b;
        assert(!b.Bind(lease,io) && b.State()==BdwGgttBindState::Quarantined && !b.SafeToRelease());}
    {MmioOwner owner;uint64_t ptes[512]{};uint32_t flush=0;
        BdwGgttMmio<MmioOwner> io(owner,ptes,&flush,66047,2);BdwGgttBind b;
        assert(!b.Bind(lease,io) && flush==0);}
    // An owned range can contain unknown firmware values. Successful retirement
    // replaces them with the pinned scratch mapping rather than replaying them.
    auto unknown=lease;unknown.expected[0]=0xba141f3d556fc1caull;unknown.expected[1]=0x45ebe0c2aa903e35ull;
    {Fake f;f.pte[0]=unknown.expected[0];f.pte[1]=unknown.expected[1];BdwGgttBind b;
        assert(b.BindTakenOver(unknown,f,0x400000));assert(b.Retire(f) && b.SafeToRelease());
        assert(f.pte[0]==0x400001 && f.pte[1]==0x400001);}
    {Fake f;f.pte[0]=unknown.expected[0];f.pte[1]=unknown.expected[1];BdwGgttBind b;
        assert(b.BindTakenOver(unknown,f,0x400000));assert(b.RetireToInvalid(f) && b.SafeToRelease());
        assert(!f.pte[0] && !f.pte[1] && f.invalidations==2);}
    {DelayedFake f;f.pte[0]=unknown.expected[0];f.pte[1]=unknown.expected[1];BdwGgttBind b;
        assert(b.BindTakenOver(unknown,f,0x400000));assert(b.RetireToInvalid(f) && b.SafeToRelease());
        assert(!f.pte[0] && !f.pte[1] && !f.pending);}
    {Fake f;f.pte[0]=unknown.expected[0];f.pte[1]=unknown.expected[1];BdwGgttBind b;
        assert(b.BindTakenOver(unknown,f,0x400000));f.pte[1]^=0x1000;const auto writes=f.writes;
        assert(!b.RetireToInvalid(f) && !b.SafeToRelease() && f.writes==writes);}
    {Fake f;f.pte[0]=unknown.expected[0];f.pte[1]=unknown.expected[1];BdwGgttBind b;
        assert(b.BindTakenOver(unknown,f,0x400000));f.failWrite=f.writes+1;
        assert(!b.RetireToInvalid(f) && !b.SafeToRelease() && b.State()==BdwGgttBindState::Quarantined);}
    {Fake f;f.pte[0]=unknown.expected[0];f.pte[1]=unknown.expected[1];BdwGgttBind b;
        assert(b.BindTakenOver(unknown,f,0x400000));f.failInvalidate=f.invalidations+1;
        assert(!b.RetireToInvalid(f) && !b.SafeToRelease() && b.State()==BdwGgttBindState::Quarantined);}
    {Fake f;f.pte[0]=unknown.expected[0];f.pte[1]=unknown.expected[1];BdwGgttBind b;
        assert(b.BindTakenOver(unknown,f,0x400000));f.idle=false;const auto writes=f.writes;
        assert(!b.RetireToInvalid(f) && f.writes==writes && !b.SafeToRelease());}
    {Fake f;BdwGgttBind b;assert(b.Bind(lease,f));const auto writes=f.writes;
        assert(!b.RetireToInvalid(f) && f.writes==writes && b.State()==BdwGgttBindState::Bound);}
    {Fake f;f.takeover=false;BdwGgttBind b;assert(!b.BindTakenOver(unknown,f,0x400000) && !f.reads && !f.writes);}
    {Fake f;BdwGgttBind b;assert(!b.BindTakenOver(lease,f,lease.dma[0]) && !f.writes);}
    {DelayedFake f;f.pte[0]=unknown.expected[0];f.pte[1]=unknown.expected[1];f.failWrite=2;BdwGgttBind b;
        assert(!b.BindTakenOver(unknown,f,0x400000) && b.SafeToRelease() && !f.pending);
        assert(f.pte[0]==0x400001 && f.pte[1]==0x400001);}
    {Fake f;f.pte[0]=unknown.expected[0];f.pte[1]=unknown.expected[1];f.failWrite=2;f.failInvalidate=1;BdwGgttBind b;
        assert(!b.BindTakenOver(unknown,f,0x400000) && !b.SafeToRelease() && b.State()==BdwGgttBindState::Quarantined);}
}
