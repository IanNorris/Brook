#include "intel_bdw_render_oneshot.h"
#include "intel_bdw_render_backend.h"
#include "intel_bdw_render_transaction.h"
#include "intel_bdw_wait_yield.h"
#include <cassert>
#include <cstdlib>
#include <vector>
using namespace brook;
namespace brook {
struct BdwExecSnapshot { bool syncOk=true,releaseOk=true;unsigned syncs=0,releases=0; };
bool BdwExecSnapshotBuildRenderImage(BdwExecSnapshot*,const BdwPpgttPage (&)[4],const BdwPpgttPage*,uint32_t,
    BdwPpgttMapping*,uint32_t,uint64_t,uint32_t,bool,uint32_t (&)[56]) { return true; }
bool BdwExecSnapshotArm(BdwExecSnapshot*,bool published) { return published; }
bool BdwExecSnapshotSyncForCpu(BdwExecSnapshot* s,bool stopped) { ++s->syncs;return stopped && s->syncOk; }
bool BdwExecSnapshotRelease(BdwExecSnapshot* s,bool stopped) { ++s->releases;return stopped && s->releaseOk; }
}
static uint32_t staging[20*1024];
static uint32_t captured[20*1024];
struct YieldFixture {
    bool may=true,audit=true,held=true,poisoned=false;unsigned step=0;
    bool MayYield(){return may && held && !poisoned;}
    void Unlock(){assert(held && step++==0);held=false;}
    void Wait(){assert(!held && step++==1);}
    void Lock(){assert(!held && step++==2);held=true;}
    bool Audit(){assert(held && step++==3);return audit;}
    void Poison(){assert(held && step++==4);poisoned=true;}
};
struct TransactionOwner {
    bool gt=true,pat=true,runOk=true,save=true,reset=true,restore=true,bad=false;
    unsigned step=0,gtAt=0,patAt=0,runAt=0,saveAt=0,resetAt=0,restoreAt=0;
    bool initializeGt(){gtAt=++step;return gt;}
    bool establishPat(){patAt=++step;return pat;}
    bool run(){runAt=++step;return runOk;}
    bool saveContext(){saveAt=++step;return save;}
    bool resetAfter(){resetAt=++step;return reset;}
    bool restorePat(){restoreAt=++step;return restore;}
    bool uncertain(){return bad;}
};
struct Allocator {
    std::vector<BdwOwnedPage> pages;
    unsigned frees=0;bool failPublish=false;
    ~Allocator(){for(auto p:pages)std::free(p.cpu);}
    bool alloc(BdwOwnedPage& p){void* data=nullptr;if(posix_memalign(&data,4096,4096))return false;
        p={0x400000+uint64_t(pages.size())*4096,static_cast<uint8_t*>(data)};pages.push_back(p);return true;}
    void free(BdwOwnedPage p){for(auto it=pages.begin();it!=pages.end();++it)if(it->cpu==p.cpu){
        std::free(it->cpu);pages.erase(it);++frees;return;}assert(false);}
    bool publish(BdwOwnedPage*,unsigned n){return n==37 && !failPublish;}
};
struct Backend {
    BdwOwnedPage* pages=nullptr;uint64_t entries[29]{};unsigned writes=0,portWrites=0,acks=0;
    bool held=true,pat=true,ready=true,idle=true,complete=true,failPort=false,loseStateOnInit=false,badEvent=false,store=false;
    bool exclusiveHandoff(){return held;} bool renderStateReady(){return ready;}
    bool admitPages(BdwOwnedPage* p,unsigned n){pages=p;return n==37;}
    bool admitExtraTables(BdwOwnedPage*,unsigned){return true;}bool publishExtraTables(BdwOwnedPage*,unsigned){return true;}
    bool gpuPatLease(){return pat;} bool preparePrivatePat(uint64_t){return pat;}
    bool readPrivatePat(uint32_t& lo,uint32_t& hi){lo=7;hi=11;return pat;}
    bool publish(BdwOwnedPage* p,unsigned n){return p==pages && n==37;}
    bool validateLease(const BdwGgttLease& l){return held && l.token==42 && l.count==29;}
    bool validateTakeover(const BdwGgttLease& l,uint64_t scratch){return validateLease(l) && scratch==0x800000;}
    bool quiescent(){return idle;}bool read(uint32_t i,uint64_t& v){if(i<65536||i>=65565)return false;v=entries[i-65536];return true;}
    bool write(uint32_t i,uint64_t v){if(i<65536||i>=65565)return false;++writes;entries[i-65536]=v;return true;}
    bool invalidate(){return held;}
    bool initializeExeclists(uint64_t hws,const BdwOwnedPage& page){if(loseStateOnInit)ready=false;
        return hws==(65536ull+24)*4096 && page.cpu==pages[24].cpu;}
    bool prepareSubmission(uint64_t d){return d==0x11000013dull;}
    bool submitDword(uint32_t){++portWrites;return !failPort;}
    bool waitCompletion(uint32_t nonce){if(!complete)return false;auto* p=pages[25].cpu;
        for(unsigned i=0;i<4;++i)p[i]=uint8_t(nonce>>(8*i));
        if(store)for(unsigned i=0;i<4;++i)pages[27].cpu[i]=uint8_t(nonce>>(8*i));
        return true;}
    bool cpuVisible(BdwOwnedPage* p,unsigned n){return p==pages && n==37;}
    bool csbReadTail(BdwOwnedPage&,uint32_t& tail){tail=1;return true;}
    bool csbReadEntry(BdwOwnedPage&,unsigned slot,uint64_t& event){event=slot==0?1:((uint64_t{badEvent?2u:1u}<<32)|0x18);return true;}
    bool csbAcknowledge(unsigned){++acks;return true;}
};
struct DeviceOwner {
    uint32_t* engine;BdwOwnedPage* pages=nullptr;bool held=true,pat=true,ready=true,restoreOk=true;
    unsigned resets=0,restores=0,acks=0,budget=1;uint32_t nonce=99,contextId=1;
    bool owned(){return held;} bool renderStateReady(){return ready;}
    bool gpuPatLease(){return pat;} bool preparePrivatePat(uint64_t){return pat;}
    bool readPrivatePat(uint32_t& lo,uint32_t& hi){lo=7;hi=11;return pat;}
    bool admitPages(BdwOwnedPage* p,unsigned n){pages=p;return n==37;}
    bool publish(BdwOwnedPage* p,unsigned n){return p==pages && n==37;}
    bool admitExtraTables(BdwOwnedPage*,unsigned n){return held && n>0;}
    bool publishExtraTables(BdwOwnedPage*,unsigned n){return held && n>0;}
    bool validateLease(const BdwGgttLease& l){return l.count==29 && l.token==42;}
    bool validateTakeover(const BdwGgttLease&,uint64_t scratch){return scratch==0x800000;}
    bool quiescent(){return true;}bool invalidateGpuTranslations(){return true;}
    bool resetRcs(){++resets;ready=false;engine[0x3a0/4]=7;return true;}
    bool restoreRenderState(){++restores;ready=restoreOk;return restoreOk;}
    bool syncForDevice(const BdwOwnedPage&){return true;}
    bool syncForCpu(const BdwOwnedPage&){return true;}
    bool completionContinue(){if(!budget--)return false;
        __atomic_store_n(reinterpret_cast<uint32_t*>(pages[25].cpu),nonce,__ATOMIC_RELAXED);
        __atomic_store_n(reinterpret_cast<uint64_t*>(pages[24].cpu+0x40),uint64_t{1},__ATOMIC_RELAXED);
        __atomic_store_n(reinterpret_cast<uint64_t*>(pages[24].cpu+0x48),(uint64_t(contextId)<<32)|0x18,__ATOMIC_RELAXED);
        __atomic_store_n(reinterpret_cast<uint32_t*>(pages[24].cpu+0x7c),1u,__ATOMIC_RELAXED);return true;}
    void relax(){}bool acknowledgeCsb(unsigned){++acks;return true;}bool retirementContinue(){return held;}
};
static uint32_t word(const BdwOwnedPage& p,unsigned i){const auto* b=p.cpu+4*i;return uint32_t(b[0])|(uint32_t(b[1])<<8)|(uint32_t(b[2])<<16)|(uint32_t(b[3])<<24);}
int main(){
    {struct PollOwner {unsigned immediate=0,yields=0;bool allowed=true;
       bool immediatePollAllowed(){++immediate;return allowed;}
       bool pollContinue(){++yields;return allowed;}} o;
     bool first=true;assert(BdwFirstGpuPoll(o,first) && !first && o.immediate==1 && o.yields==0);
     assert(BdwFirstGpuPoll(o,first) && o.immediate==1 && o.yields==1);
     o.allowed=false;assert(!BdwFirstGpuPoll(o,first) && o.yields==2);
     first=true;assert(!BdwFirstGpuPoll(o,first) && !first && o.immediate==2);}
    {YieldFixture y;assert(BdwYieldGpuWait(y) && y.held && !y.poisoned && y.step==4);}
    {YieldFixture y;y.audit=false;assert(!BdwYieldGpuWait(y) && y.held && y.poisoned && y.step==5);}
    {YieldFixture y;y.may=false;assert(!BdwYieldGpuWait(y) && y.held && !y.step);}
    {TransactionOwner o;const auto r=BdwRunOwnedRenderTransaction(o);
     assert(r.gtReady && r.patReady && r.runComplete && r.contextSaved && r.resetComplete && r.patRestored && !r.uncertain);
     assert(o.gtAt==1 && o.patAt==2 && o.runAt==3 && o.saveAt==4 && o.resetAt==5 && o.restoreAt==6);}
    {TransactionOwner o;o.runOk=false;const auto r=BdwRunOwnedRenderTransaction(o);
     assert(!r.runComplete && !r.contextSaved && r.resetComplete && r.patRestored && !r.uncertain);
     assert(!o.saveAt && o.resetAt==4 && o.restoreAt==5);}
    {TransactionOwner o;o.save=false;o.reset=false;const auto r=BdwRunOwnedRenderTransaction(o);
     assert(r.runComplete && !r.contextSaved && r.resetAttempted && !r.resetComplete && !r.patRestored && r.uncertain && !o.restoreAt);}
    {TransactionOwner o;o.pat=false;o.bad=true;const auto r=BdwRunOwnedRenderTransaction(o);
     assert(r.gtReady && !r.patReady && r.uncertain && !o.runAt && !o.resetAt);}
    {TransactionOwner o;o.gt=false;const auto r=BdwRunOwnedRenderTransaction(o);
     assert(!r.gtReady && !r.uncertain && !o.patAt && !o.resetAt);}
    {TransactionOwner o;o.restore=false;const auto r=BdwRunOwnedRenderTransaction(o);
     assert(r.contextSaved && r.resetComplete && !r.patRestored && r.uncertain);}
    {TransactionOwner o;o.bad=true;const auto r=BdwRunOwnedRenderTransaction(o);
     assert(r.resetComplete && !r.patRestored && r.uncertain && !o.restoreAt);}
    {alignas(4096) uint32_t page[1024];for(auto& value:page)value=0xa5a5a5a5;
     assert(!BdwBuildRenderNullState(0,page) && !BdwBuildRenderNullState(0x10001,page));
     for(auto value:page)assert(value==0xa5a5a5a5);
     assert(BdwBuildRenderNullState(0x10000000,page));
     const unsigned offsets[]={0x798,0x7a4,0x7ac,0x7bc};
     for(auto off:offsets)assert(page[off/4]==0x10000001 && page[off/4+1]==0);
     assert(page[0]==0x7a000004 && page[1]==0x01000000);}
    {Allocator a;BdwRenderOneShot shot;captured[4096]=0x1289abcd;
     assert(shot.Prepare(a,65565,43,100,staging,false,&captured,2));
     assert(shot.InspectPrepared(a,[](const BdwOwnedPage* p,unsigned count,const BdwRenderPlan& plan){
        assert(count==37 && word(p[4],0)==0x1289abcd && word(p[1],3)==0x000f0008);
        assert(plan.descriptor==((uint64_t{2}<<32)|(65565ull<<12)|0x13d));
        assert(word(p[1],9)==(65565u+20)*4096 && word(p[1],27)==(plan.waGgtt|2));return true;}));
     assert(shot.ReleasePrepared(a));}
    {Allocator a;BdwRenderOneShot shot;assert(shot.Prepare(a,65536,42,0x12345678,staging));
     assert(shot.PublishPrepared(a));assert(shot.InspectPrepared(a,[](const BdwOwnedPage* p,unsigned count,const BdwRenderPlan& plan){
        assert(count==37 && plan.lease.count==29 && plan.lease.dma[28]==p[36].dma && word(p[0],0)==0 && word(p[20],0)==0x7a000004);
        assert(word(p[20],1)==0x00344c1c && word(p[20],39)==0x18800001 && word(p[20],40)==(65536u+28)*4096);
        assert(word(p[20],6)==0x7a000004 && word(p[20],7)==0x00100000 && word(p[20],12)==0x11000011);
        assert(word(p[20],19)==0x7300 && word(p[20],20)==0x08300830 && word(p[20],32)==0x7a000004);
        assert(word(p[20],48)==0x7a000004 && word(p[20],49)==0x01104080 &&
               word(p[20],50)==plan.completionGgtt && word(p[20],52)==0x12345678);
        assert(word(p[36],0)==0x7a000004 && word(p[36],0x798/4)==(65536u+28)*4096+1 && word(p[25],0)==0);return true;}));
     assert(shot.ReleasePrepared(a) && a.frees==37);}
    {Allocator a;Backend b;BdwRenderOneShot shot;
     assert(!shot.Prepare(a,65536,42,99,staging,false,nullptr,1,false,true));
     assert(shot.Prepare(a,65536,42,99,staging,false,nullptr,1,true,true));
     assert(shot.InspectPrepared(a,[](const BdwOwnedPage* p,unsigned,const BdwRenderPlan&){
        const auto* pt=reinterpret_cast<const uint64_t*>(p[35].cpu);
        assert(pt[17]==(p[27].dma|0x83));
        assert(word(p[36],0)==0x10000002 && word(p[36],1)==0x11000 && word(p[36],2)==0 &&
               word(p[36],3)==99 && word(p[36],4)==0x05000000);return true;}));
     b.store=true;assert(shot.ExecuteTakenOver(a,b,0x800000));}
    {Allocator a;Backend b;BdwRenderOneShot shot;
     assert(shot.Prepare(a,65536,42,99,staging,false,nullptr,1,true,true));
     assert(!shot.ExecuteTakenOver(a,b,0x800000) && shot.State()==BdwOneShotState::Quarantined);}
    {Allocator a;BdwRenderOneShot shot;
     assert(shot.Prepare(a,65536,42,0x12345678,staging,false,nullptr,1,true));
     assert(shot.InspectPrepared(a,[](const BdwOwnedPage* p,unsigned count,const BdwRenderPlan&){
        assert(count==37 && word(p[20],39)==0x18800101 && word(p[20],40)==0x10000 && word(p[20],41)==0);
        assert(word(p[36],0)==0x05000000 && word(p[36],1)==0);
        const auto* pt=reinterpret_cast<const uint64_t*>(p[35].cpu);
        assert(pt[16]==(p[36].dma|0x81));return true;}));
     assert(shot.ReleasePrepared(a) && a.frees==37);}
    {uint32_t gt2[20]{},gt3[20]{};BdwBuildRenderContextWa(false,gt2);BdwBuildRenderContextWa(true,gt3);
     for(unsigned i=0;i<20;++i)if(i!=8)assert(gt2[i]==gt3[i]);
     assert(gt2[8]==0x08300830 && gt3[8]==0x48304830);}
    {Allocator a;Backend b;BdwRenderOneShot shot;assert(shot.Prepare(a,65536,42,99,staging));b.ready=false;
     assert(!shot.ExecuteTakenOver(a,b,0x800000) && b.writes==0 && b.portWrites==0 && shot.ReleasePrepared(a));}
    {Allocator a;Backend b;BdwRenderOneShot shot;assert(shot.Prepare(a,65536,42,99,staging));
     assert(shot.ExecuteTakenOver(a,b,0x800000) && b.writes==29 && b.portWrites==4);
     assert(shot.State()==BdwOneShotState::CompleteRetained && !shot.ReleasePrepared(a));
     assert(!shot.CopyRetiredContext(b,captured));
     assert(shot.PollContextSwitch(b)==BdwCsbPoll::Complete && b.acks==2 &&
            shot.State()==BdwOneShotState::ContextCompleteRetained && !shot.ReleasePrepared(a));
     b.pages[3].cpu[7]=0x5a;assert(shot.CopyRetiredContext(b,captured));
     assert(reinterpret_cast<uint8_t*>(captured)[3*4096+7]==0x5a);
     b.pat=false;assert(!shot.CopyRetiredContext(b,captured) && shot.State()==BdwOneShotState::Quarantined);}
    {Allocator a;Backend b;BdwRenderOneShot shot;assert(shot.Prepare(a,65536,42,99,staging));
     assert(shot.ExecuteTakenOver(a,b,0x800000));assert(shot.PollContextSwitch(b)==BdwCsbPoll::Complete);
     assert(!shot.RetireAfterReset(b,false) && !shot.ReleaseStopped(a));
     assert(shot.RetireAfterReset(b,true) && shot.State()==BdwOneShotState::StoppedRetained);
     for(auto pte:b.entries)assert(pte==0);
     assert(shot.ReleaseStopped(a) && a.frees==37);}
    {Allocator a,wrong;Backend b;BdwRenderOneShot shot;BdwExecSnapshot snapshot;BdwPpgttMapping workspace[1]{};
     assert(shot.Prepare(a,65536,42,99,staging));
     assert(shot.AttachSnapshot(a,&snapshot,workspace,1) && shot.PublishPrepared(a) && shot.ArmSnapshot(a));
     assert(!shot.ReleaseStoppedSnapshot(a) && !shot.ReleaseStopped(a));
     assert(shot.ExecuteTakenOver(a,b,0x800000) && shot.PollContextSwitch(b)==BdwCsbPoll::Complete);
     assert(shot.RetireAfterReset(b,true) && !shot.ReleaseStoppedSnapshot(wrong) && !shot.ReleaseStopped(a));
     snapshot.syncOk=false;assert(!shot.ReleaseStoppedSnapshot(a) && snapshot.syncs==1 && snapshot.releases==0 && !shot.ReleaseStopped(a));
     snapshot.syncOk=true;snapshot.releaseOk=false;
     assert(!shot.ReleaseStoppedSnapshot(a) && snapshot.syncs==2 && snapshot.releases==1 && !shot.ReleaseStopped(a));
     snapshot.releaseOk=true;assert(shot.ReleaseStoppedSnapshot(a) && snapshot.syncs==3 && snapshot.releases==2);
     assert(!shot.ReleaseStoppedSnapshot(a) && shot.ReleaseStopped(a) && a.frees==37);}
    {Allocator a;Backend b;BdwRenderOneShot shot;assert(shot.Prepare(a,65536,42,99,staging));
     assert(shot.ExecuteTakenOver(a,b,0x800000));b.entries[0]^=0x1000;const auto writes=b.writes;
     assert(!shot.RetireAfterReset(b,true) && b.writes==writes && !shot.ReleaseStopped(a) &&
            shot.State()==BdwOneShotState::Quarantined && !shot.Recycle(a));}
    {Allocator a;Backend b;BdwRenderOneShot shot;assert(shot.Prepare(a,65536,42,99,staging));b.complete=false;
     assert(!shot.ExecuteTakenOver(a,b,0x800000) && b.portWrites==4 && shot.State()==BdwOneShotState::Quarantined && !shot.ReleasePrepared(a));}
    {Allocator a;Backend b;BdwRenderOneShot shot;assert(shot.Prepare(a,65536,42,99,staging));b.failPort=true;
     assert(!shot.ExecuteTakenOver(a,b,0x800000) && b.portWrites==1 && shot.State()==BdwOneShotState::Quarantined && !shot.ReleasePrepared(a));}
    {Allocator a;Backend b;BdwRenderOneShot shot;assert(shot.Prepare(a,65536,42,99,staging));b.loseStateOnInit=true;
     assert(!shot.ExecuteTakenOver(a,b,0x800000) && b.writes==29 && b.portWrites==0 && shot.State()==BdwOneShotState::Quarantined);}
    {Allocator a;Backend b;BdwRenderOneShot shot;assert(shot.Prepare(a,65536,42,99,staging));
     assert(shot.ExecuteTakenOver(a,b,0x800000));b.badEvent=true;
     assert(shot.PollContextSwitch(b)==BdwCsbPoll::Fault && b.acks==1 && shot.State()==BdwOneShotState::Quarantined);}
    {Allocator a;uint64_t ptes[512]{};uint32_t flush=0,engine[1024]{};DeviceOwner owner{engine};
     BdwRenderDeviceBackend<DeviceOwner> b(owner,ptes,&flush,engine,65536,42,0x800000);BdwRenderOneShot shot;
     BdwRenderExecutionBackend& device=b;
     assert(shot.Prepare(a,65536,42,99,staging));assert(shot.ExecuteTakenOver(a,device,0x800000));
     assert(owner.resets==1 && owner.restores==1 && b.ggttWriteAttempts()==29 && b.portWriteAttempts()==4 && flush==1);
     assert(engine[0x80/4]==(65536u+24)*4096 && engine[0x230/4]==0x1000013d);
     assert(shot.PollContextSwitch(device)==BdwCsbPoll::Complete && owner.acks==2);}
    {uint64_t ptes[512]{};uint32_t flush=0,engine[1024]{};DeviceOwner owner{engine};
     BdwRenderDeviceBackend<DeviceOwner> b(owner,ptes,&flush,engine,65536,42,0x800000);BdwRenderExecutionBackend& device=b;
     BdwOwnedPage base[37]{},extra[2]{};
     for(unsigned i=0;i<37;++i)base[i]={0x1000000ull+4096ull*i,reinterpret_cast<uint8_t*>(0x10000000ull+4096ull*i)};
     extra[0]={base[0].dma,reinterpret_cast<uint8_t*>(0x20000000)};extra[1]={0x3001000,reinterpret_cast<uint8_t*>(0x20001000)};
     assert(!device.admitExtraTables(extra,2) && device.admitPages(base,37));
     assert(!device.admitExtraTables(extra,2));extra[0].dma=0x800000;
     assert(!device.admitExtraTables(extra,2));extra[0].dma=0x3000000;
     extra[1].cpu=extra[0].cpu;assert(!device.admitExtraTables(extra,2));extra[1].cpu=reinterpret_cast<uint8_t*>(0x20001000);
     assert(device.admitExtraTables(extra,2) && !device.admitExtraTables(extra,2));
     assert(!device.publishExtraTables(extra,1) && device.publishExtraTables(extra,2));
     extra[0].dma+=4096;assert(!device.publishExtraTables(extra,2));extra[0].dma-=4096;
     owner.held=false;assert(!device.publishExtraTables(extra,2));}
    {Allocator a;uint64_t ptes[512]{};uint32_t flush=0,engine[1024]{};DeviceOwner owner{engine};owner.restoreOk=false;
     BdwRenderDeviceBackend<DeviceOwner> b(owner,ptes,&flush,engine,65536,42,0x800000);BdwRenderOneShot shot;
     assert(shot.Prepare(a,65536,42,99,staging));assert(!shot.ExecuteTakenOver(a,b,0x800000));
     assert(owner.resets==1 && owner.restores==1 && b.ggttWriteAttempts()==29 && b.portWriteAttempts()==0 &&
            shot.State()==BdwOneShotState::Quarantined);}
    {Allocator a;uint64_t ptes[512]{};uint32_t flush=0,engine[1024]{};DeviceOwner owner{engine};owner.contextId=2;
     BdwRenderDeviceBackend<DeviceOwner> b(owner,ptes,&flush,engine,65565,42,0x800000,2);BdwRenderOneShot shot;
     captured[4096]=0x47505531;assert(shot.Prepare(a,65565,42,99,staging,false,&captured,2));
     assert(shot.ExecuteTakenOver(a,b,0x800000));assert(shot.PollContextSwitch(b)==BdwCsbPoll::Complete);
     assert(shot.CopyRetiredContext(b,captured) && captured[4096]==0x47505531);
     assert(owner.resets==1 && owner.restores==1 && owner.acks==2 && b.portWriteAttempts()==4);}
}
