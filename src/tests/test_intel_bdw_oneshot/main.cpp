#include "intel_bdw_oneshot.h"
#include "intel_bdw_live_backend.h"
#include "intel_bdw_csb.h"
#include "intel_bdw_bcs_init.h"
#include "intel_bdw_bcs_mmio.h"
#include "intel_bdw_bcs_reset.h"
#include "intel_bdw_rcs_reset_mmio.h"
#include "intel_bdw_pat.h"
#include "intel_bdw_tlb.h"
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
using namespace brook;
struct MmioOwner {
    bool held=true,idle=true,prepared=true,synchronized=true,memoryReady=true;uint32_t producer=5;
    bool owned(){return held;}
    bool quiescent(){return idle;}
    bool csbPrepared(uint64_t hws){return prepared && hws==0x10006000;}
    bool resetVerified(){return synchronized;}
    bool submissionReady(uint64_t desc){return held && synchronized && desc==0x11000013dull;}
    bool allEnginesQuiescent(){return idle;}
    bool safePatTransition(uint32_t,uint32_t,uint32_t,uint32_t){return prepared;}
    bool drainAndInvalidate(){return held && synchronized;}
    bool prepareCsbMemory(uint64_t hws){return memoryReady && synchronized && csbPrepared(hws);}
    bool csbMemoryProducer(uint64_t hws,uint32_t& value){value=producer;return memoryReady && synchronized && csbPrepared(hws);}
    bool monotonicMs(uint64_t& v){v=0;return true;} void relax(){}
};
struct PatIo {
    uint32_t low=0x03030303,high=0x03030303;unsigned writes=0,reads=0,failWrite=0,failDrain=0,drains=0;
    bool held=true,idle=true,safe=true,change=false;
    bool owned(){return held;} bool allEnginesQuiescent(){return idle;}
    bool safePatTransition(uint32_t,uint32_t,uint32_t,uint32_t){return safe;}
    bool readPrivatePat(uint32_t& lo,uint32_t& hi){lo=low;hi=high;if(change && ++reads==2)lo^=1;return true;}
    bool writePatLow(uint32_t v){low=v;return ++writes!=failWrite;}
    bool writePatHigh(uint32_t v){high=v;return ++writes!=failWrite;}
    bool drainAndInvalidate(){return ++drains!=failDrain;}
};
struct LiveOwner {
    bool held=true,pat=true,idle=true,admitted=true;unsigned flushes=0,budget=1;
    bool owned(){return held;}bool gpuPatLease(){return pat;}
    bool preparePrivatePat(uint64_t){return pat;}
    bool readPrivatePat(uint32_t& lo,uint32_t& hi){lo=7;hi=11;return pat;}
    bool admitPages(BdwOwnedPage*,unsigned n){return admitted && n==19;}
    bool publish(BdwOwnedPage*,unsigned n){return n==19;}
    bool validateLease(const BdwGgttLease&){return true;}
    bool validateTakeover(const BdwGgttLease&,uint64_t){return true;}
    bool quiescent(){return idle;}bool invalidateGpuTranslations(){return true;}
    bool resetBcs(){return true;}bool syncForDevice(const BdwOwnedPage&){return true;}
    bool syncForCpu(const BdwOwnedPage&){++flushes;return true;}
    bool completionContinue(){return budget--!=0;}bool retirementContinue(){return budget--!=0;}
    void relax(){}bool acknowledgeCsb(unsigned){return true;}
};
struct TlbIo {
    bool held=true,idle=true,busy=false,stuck=false,badClock=false;unsigned writes=0,failWrite=0;uint64_t time=0;
    bool owned(){return held;} bool allEnginesQuiescent(){return idle;}
    bool monotonicMs(uint64_t& v){if(badClock)return false;v=time++;return true;}void relax(){}
    bool readReg(uint32_t,uint32_t& v){v=(busy || (stuck && writes))?1:0;return true;}
    bool writeReg(uint32_t,uint32_t v){assert(v==1);return ++writes!=failWrite;}
};
struct ResetIo {
    bool held=true,idle=true,ready=true,complete=true,failWrite=false,modeIdle=true,failModeRead=false;unsigned writes=0,cancels=0,stops=0;
    uint64_t ticks=0;uint32_t ctl=0,domain=0,mode=0,base=0x22000,domainBit=8;
    bool owned(){return held;} bool quiescent(){return idle;}
    bool monotonicMs(uint64_t& v){v=ticks++;return true;} void relax(){}
    bool readReg(uint32_t r,uint32_t& v){if(r==base+0xd0)v=ctl;else if(r==0x941c)v=domain;else if(r==base+0x9c){if(failModeRead)return false;v=mode;}else return false;return true;}
    bool writeReg(uint32_t r,uint32_t v){++writes;if(failWrite)return false;
        if(r==base+0xd0 && v==0x10001){if(ready)ctl|=2;return true;}
        if(r==base+0xd0 && v==0x10000){++cancels;ctl&=~1u;return true;}
        if(r==base+0x9c && v==0x01000100){++stops;if(modeIdle)mode|=0x200;return true;}
        if(r==0x941c && v==domainBit){domain=complete?0:domainBit;return true;}return false;}
};
struct Fake : BdwBcsExecutionBackend {
    std::vector<BdwOwnedPage> live;uint64_t slots[8]{};uint32_t elsp[4]{};
    unsigned allocations=0,frees=0,engineWrites=0,portWrites=0,ggttWrites=0,ggttReads=0,invalidates=0,failAlloc=0,failPort=0,failGgttWrite=0;
    unsigned alterRead=0,initStep=0,failInitStep=0,patReads=0,losePatOnWrite=0,losePatOnPort=0;uint32_t patLow=7,patHigh=11;bool changePat=false;
    unsigned csbAcks=0,csbAckSlots[6]{};bool csbFailAck=false,csbFailTail=false,csbFailEntry=false;
    bool handoff=true,execlist=true,modeReadable=true,modeStable=true,ringRunning=true,csbReady=true,csbSynced=true;
    bool admitted=true,pat=true,published=false,failHws=false,complete=true,idle=true,corruptSource=false,takeover=true,portReady=true;
    bool scanout=false;unsigned cpuVisibilityChecks=0;
    BdwBcsOneShot* reentrant=nullptr;unsigned aliasAt=0,aliasKind=0;
    ~Fake(){for(auto p:live)std::free(p.cpu);}
    bool alloc(BdwOwnedPage& out){if(++allocations==failAlloc)return false;void* p=nullptr;assert(!posix_memalign(&p,4096,4096));
        out={0x200000+uint64_t(allocations)*0x3000,static_cast<uint8_t*>(p)};live.push_back(out);
        if(allocations==aliasAt){if(aliasKind==1)out.dma=live[0].dma;else out.cpu=live[0].cpu;}
        if(reentrant)assert(!reentrant->ReleasePrepared(*this));
        return true;}
    void free(BdwOwnedPage p){if(reentrant){assert(!reentrant->ReleasePrepared(*this));assert(!reentrant->Execute(*this));}
        for(auto i=live.begin();i!=live.end();++i)if(i->cpu==p.cpu){std::free(i->cpu);live.erase(i);++frees;return;}assert(false);}
    bool exclusiveHandoff(){return handoff;}
    bool execlistReady(){return modeReadable && modeStable && execlist && ringRunning && csbReady;}
    bool gpuPatLease(){return pat && handoff;}
    bool preparePrivatePat(uint64_t descriptor){return pat && handoff && (descriptor&0x119)==0x119;}
    bool readPrivatePat(uint32_t& low,uint32_t& high){low=patLow;high=patHigh;if(changePat && ++patReads==2)low^=1;return pat;}
    bool admitPages(BdwOwnedPage*,unsigned n){assert(n==live.size());return admitted;}
    bool publish(BdwOwnedPage*,unsigned n){assert(n==live.size());if(reentrant)assert(!reentrant->ReleasePrepared(*this));published=true;return true;}
    bool validateLease(const BdwGgttLease& l){return handoff && l.token==99 && l.count==8;}
    bool validateTakeover(const BdwGgttLease& l,uint64_t scratch){return takeover && validateLease(l) && scratch==0x600000;}
    bool quiescent(){return idle;}
    bool read(uint32_t i,uint64_t& v){assert(i>=65536 && i<65544);v=slots[i-65536];if(++ggttReads==alterRead)v^=0x1000;return true;}
    bool write(uint32_t i,uint64_t v){assert(i>=65536 && i<65544);++ggttWrites;slots[i-65536]=v;if(ggttWrites==losePatOnWrite)pat=false;return ggttWrites!=failGgttWrite;}
    bool invalidate(){++invalidates;return true;}
    bool programHws(uint64_t addr){++engineWrites;assert(published && addr==0x10006000);return !failHws;}
    bool initializeExeclists(uint64_t addr,const BdwOwnedPage& hwsPage){
        struct Engine {
            Fake& f;const BdwOwnedPage& page;uint32_t mode=0,mi=0,hws=0;
            bool owned(){return f.handoff;}
            bool quiescent(){return f.idle;}
            bool csbPrepared(uint64_t){return f.csbReady;}
            bool synchronizeCsb(uint64_t){return f.csbSynced && BdwCsbInitializeForReset(page.cpu,[&](uint8_t* p){return p==page.cpu && f.published;});}
            bool writeReg(uint32_t reg,uint32_t value){
                if(++f.initStep==f.failInitStep)return false;
                if(reg==0x22098)return value==0xffffffffu;
                if(reg==0x2229c){mode=value&0xffff;return true;}
                if(reg==0x2209c){mi=0;return true;}
                if(reg==0x22080){hws=value;++f.engineWrites;return !f.failHws;}
                return false;
            }
            bool readReg(uint32_t reg,uint32_t& value){
                if(++f.initStep==f.failInitStep)return false;
                if(!f.modeReadable)return false;
                if(reg==0x2229c)value=mode;else if(reg==0x2209c)value=mi;
                else if(reg==0x22080)value=hws;else return false;
                return true;
            }
        } engine{*this,hwsPage};
        assert(published && addr==0x10006000 && live.size()>=19 &&
               hwsPage.cpu==live[6].cpu && hwsPage.dma==live[6].dma &&
               slots[6]==(hwsPage.dma|1));
        return BdwInitializeBcsExeclists(engine,addr);
    }
    bool prepareSubmission(uint64_t descriptor){return handoff && portReady && descriptor==0x11000013dull;}
    bool submitDword(uint32_t v){assert(portWrites<4);elsp[portWrites++]=v;if(portWrites==losePatOnPort)pat=false;return portWrites!=failPort;}
    uint8_t* fromDma(uint64_t addr){addr&=~uint64_t(4095);for(auto p:live)if(p.dma==addr)return p.cpu;assert(false);return nullptr;}
    uint64_t read64(uint8_t* p,unsigned index){uint64_t v=0;for(unsigned i=0;i<8;++i)v|=uint64_t(p[index*8+i])<<(8*i);return v;}
    uint32_t read32(uint8_t* p,unsigned index){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(p[index*4+i])<<(8*i);return v;}
    uint8_t* ggtt(uint32_t address){unsigned slot=(address>>12)-65536;assert(slot<8);return fromDma(slots[slot])+(address&4095);}
    uint64_t pte(uint64_t root,uint64_t address){
        auto* p=fromDma(root);for(unsigned shift:{39,30,21}){uint64_t e=read64(p,(address>>shift)&511);assert((e&3)==3);p=fromDma(e);}
        return read64(p,(address>>12)&511);}
    uint8_t* translate(uint64_t root,uint64_t address,bool writable){
        uint64_t e=pte(root,address);assert((e&0x81)==0x81 && bool(e&2)==writable);return fromDma(e)+(address&4095);}
    bool waitCompletion(uint32_t nonce){if(!complete)return false;
        assert(portWrites==4 && elsp[0]==0 && elsp[1]==0 && elsp[2]==1 && elsp[3]==0x1000013d);
        auto* context=ggtt((elsp[3]&~4095u)+4096);uint64_t pml4=(uint64_t(read32(context,49))<<32)|read32(context,51);
        auto* ring=ggtt(read32(context,9));assert(read32(context,7)==56 && read32(ring,0)==0x13244002 &&
            read32(ring,1)==0xd0 && read32(ring,2)==0 && read32(ring,3)==0 && read32(ring,4)==0x04000000 &&
            read32(ring,5)==0x18800101 && read32(ring,8)==0x13004002);
        auto* batch=translate(pml4,uint64_t(read32(ring,6))|(uint64_t(read32(ring,7))<<32),false);
        assert(read32(batch,0)==0x54f00008 && read32(batch,10)==0x05000000);
        if(corruptSource){uint32_t changed=read32(batch,8)+256;std::memcpy(batch+32,&changed,4);}
        uint64_t src=uint64_t(read32(batch,8))|(uint64_t(read32(batch,9))<<32);
        uint64_t dst=uint64_t(read32(batch,4))|(uint64_t(read32(batch,5))<<32);
        unsigned width=(read32(batch,3)&0xffff)*4,height=read32(batch,3)>>16;
        if(scanout){
            assert(src==BdwBcsSourceAddress && dst==BdwBcsScanoutAddress && width==64 && height==1);
            assert(pte(pml4,src)==0x400081 && pte(pml4,dst)==0xdd80001b);
        }else for(unsigned row=0;row<height;++row)
            std::memcpy(translate(pml4,dst+row*(read32(batch,1)&0xffff),true),translate(pml4,src+row*read32(batch,7),false),width);
        auto* done=ggtt(read32(ring,9)&~7u);uint32_t written=read32(ring,11);assert(written==nonce);std::memcpy(done,&written,4);return true;}
    bool cpuVisible(BdwOwnedPage*,unsigned n){++cpuVisibilityChecks;return n==19;}
    bool csbReadTail(BdwOwnedPage& hws,uint32_t& tail){if(reentrant)assert(reentrant->PollContextSwitch(*this)==BdwCsbPoll::Fault);
        return !csbFailTail && BdwCsbReadProducer(hws.cpu,tail);}
    bool csbReadEntry(BdwOwnedPage& hws,unsigned i,uint64_t& event){return !csbFailEntry && BdwCsbReadRecord(hws.cpu,i,event);}
    bool csbAcknowledge(unsigned slot){assert(csbAcks<6);csbAckSlots[csbAcks++]=slot;return !csbFailAck;}
    bool retirementContinue(){return true;}
    void event(unsigned index,uint64_t value){assert(index<6 && live.size()>=19);std::memcpy(live[6].cpu+0x40+8*index,&value,8);}
    void tail(uint32_t value){assert(live.size()>=19);std::memcpy(live[6].cpu+0x7c,&value,4);}
};
int main(){
    { LiveOwner owner;uint64_t ptes[512]{};uint32_t flush=0,engine[1024]{};
      BdwOwnedPage pages[19]{};alignas(4096) uint8_t completion[4096]{};
      for(unsigned i=0;i<19;++i)pages[i].dma=0x200000+4096*i;
      pages[7].cpu=completion;BdwBcsDeviceBackend<LiveOwner> backend(owner,ptes,&flush,engine,65536,99,0x600000);
      assert(backend.admitPages(pages,19) && !backend.admitPages(pages,19));
      BdwGgttLease lease{};lease.token=99;lease.count=8;
      for(unsigned i=0;i<8;++i){lease.index[i]=65536+i;lease.dma[i]=pages[i].dma;}
      assert(backend.validateLease(lease) && backend.validateTakeover(lease,0x600000));
      lease.dma[3]^=4096;assert(!backend.validateLease(lease));lease.dma[3]^=4096;
      assert(backend.write(65536,pages[0].dma|1) && backend.invalidate() && ptes[0]==(pages[0].dma|1));
      uint32_t nonce=0x12345678;std::memcpy(completion,&nonce,4);assert(backend.waitCompletion(nonce) && owner.flushes==1);
      owner.held=false;assert(!backend.write(65536,0) && !backend.invalidate()); }
    { TlbIo io;BdwGen8TlbInvalidate inv;assert(inv.Run(io) && io.writes==4 && !inv.Uncertain());
      assert(inv.Run(io) && io.writes==8); }
    { TlbIo io;io.busy=true;BdwGen8TlbInvalidate inv;assert(!inv.Run(io) && !io.writes && !inv.Uncertain()); }
    { TlbIo io;io.failWrite=2;BdwGen8TlbInvalidate inv;assert(!inv.Run(io) && inv.Uncertain());
      io.failWrite=0;assert(!inv.Run(io) && io.writes==2); }
    { TlbIo io;io.stuck=true;BdwGen8TlbInvalidate inv;assert(!inv.Run(io) && inv.Uncertain()); }
    { uint32_t regs[1024]{};MmioOwner owner;BdwGen8TlbMmio<MmioOwner> mmio(owner,regs);
      uint32_t v=0;assert(mmio.writeReg(0x426c,1) && mmio.readReg(0x426c,v) && v==1);
      assert(!mmio.writeReg(0x4268,1) && !mmio.writeReg(0x426c,2) && !mmio.readReg(0x40e0,v)); }
    { uint32_t regs[1024]{};regs[0xe0/4]=regs[0xe4/4]=0x03030303;
      MmioOwner owner;BdwPatMmio<MmioOwner> mmio(owner,regs);BdwPatLease lease;
      assert(!mmio.writePatLow(7));assert(lease.Establish(mmio,0x11000013dull));
      assert(regs[0xe0/4]==0x03030307 && regs[0xe4/4]==0x0303030b);
      assert(!mmio.writePatLow(0x1234));owner.held=false;assert(!mmio.writePatHigh(0x03030303)); }
    { uint32_t regs[1024]{};regs[0xe0/4]=regs[0xe4/4]=0x03030303;
      MmioOwner owner;BdwPatMmio<MmioOwner> legacy(owner,regs);BdwPatLease rejected;
      assert(!rejected.Establish(legacy,0x11000013dull,true));
      BdwPatMmio<MmioOwner> scanout(owner,regs,true);BdwPatLease lease;
      assert(lease.Establish(scanout,0x11000013dull,true) && regs[0xe0/4]==0x00030307);
      assert(lease.Release(scanout) && regs[0xe0/4]==0x03030303 && regs[0xe4/4]==0x03030303); }
    { PatIo io;BdwPatLease lease;assert(lease.Establish(io,0x11000013dull));
      assert(lease.State()==BdwPatLeaseState::Active && io.low==0x03030307 && io.high==0x0303030b && io.drains==1);
      assert(lease.Release(io) && lease.State()==BdwPatLeaseState::Restored && io.low==0x03030303 && io.high==0x03030303 && io.drains==2);
      assert(!lease.Establish(io,0x11000013dull)); }
    { PatIo io;BdwPatLease lease;assert(lease.Establish(io,0x11000013dull,true));
      assert(io.low==0x00030307 && io.high==0x0303030b);
      assert(!BdwLegacyBcsPatCompatible(0x11000013dull,0x03030307,io.high,true));
      assert(lease.Release(io) && io.low==0x03030303 && io.high==0x03030303); }
    { PatIo io,other;BdwPatLease lease;assert(lease.Establish(io,0x11000013dull));
      assert(!lease.Release(other) && lease.State()==BdwPatLeaseState::Active);
      io.idle=false;assert(!lease.Release(io) && lease.State()==BdwPatLeaseState::Quarantined && io.writes==2); }
    { PatIo io;BdwPatLease lease;assert(lease.Establish(io,0x11000013dull));
      io.failDrain=2;assert(!lease.Release(io) && lease.State()==BdwPatLeaseState::Quarantined); }
    { PatIo io;io.safe=false;BdwPatLease lease;assert(!lease.Establish(io,0x11000013dull));
      assert(lease.State()==BdwPatLeaseState::Fresh && !io.writes); }
    { PatIo io;io.change=true;BdwPatLease lease;assert(!lease.Establish(io,0x11000013dull));
      assert(lease.State()==BdwPatLeaseState::Fresh && !io.writes); }
    { PatIo io;io.failWrite=2;BdwPatLease lease;assert(!lease.Establish(io,0x11000013dull));
      assert(lease.State()==BdwPatLeaseState::Restored && io.low==0x03030303 && io.high==0x03030303); }
    { PatIo io;io.failDrain=1;BdwPatLease lease;assert(!lease.Establish(io,0x11000013dull));
      assert(lease.State()==BdwPatLeaseState::Restored && io.drains==2); }
    { PatIo io;io.failWrite=2;io.failDrain=1;BdwPatLease lease;assert(!lease.Establish(io,0x11000013dull));
      assert(lease.State()==BdwPatLeaseState::Quarantined); }
    { constexpr uint64_t legacy=0x11000013dull;
      assert(BdwLegacyBcsPatCompatible(legacy,0x03030307,0x0303030b));
      assert(!BdwLegacyBcsPatCompatible(legacy,0x03030303,0x03030303));
      assert(!BdwLegacyBcsPatCompatible(legacy&~8ull,0x07,0x0b));
      assert(!BdwLegacyBcsPatCompatible(legacy,0x07,0x03)); }
    { uint32_t regs[1024]{};MmioOwner owner;BdwBcsMmio<MmioOwner> port(owner,regs);
      assert(port.prepareSubmission(0x11000013dull));
      assert(port.submitDword(0) && port.submitDword(0) && port.submitDword(1) && port.submitDword(0x1000013d));
      assert(regs[0x230/4]==0x1000013d && !port.submitDword(0) && !port.prepareSubmission(0x11000013dull)); }
    { uint32_t regs[1024]{};MmioOwner owner;BdwBcsMmio<MmioOwner> port(owner,regs);
      assert(port.prepareSubmission(0x11000013dull));assert(!port.submitDword(1) && !port.submitDword(0)); }
    { uint32_t regs[1024]{};MmioOwner owner;BdwBcsMmio<MmioOwner> port(owner,regs);
      assert(port.prepareSubmission(0x11000013dull) && port.submitDword(0));owner.held=false;
      assert(!port.submitDword(0));owner.held=true;assert(!port.submitDword(0)); }
    { uint32_t regs[1024]{};regs[0x234/4]=0xffffffffu;MmioOwner owner;BdwBcsMmio<MmioOwner> port(owner,regs);
      assert(port.prepareSubmission(0x11000013dull) && port.submitDword(0) && port.submitDword(0) && port.submitDword(1));
      assert(!port.submitDword(0x1000013d) && !port.submitDword(0)); }
    { alignas(4096) uint8_t page[4096]{};unsigned published=0;
      assert(!BdwCsbInitializeForReset(page+1,[&](uint8_t*){++published;return true;}) && !published);
      assert(BdwCsbInitializeForReset(page,[&](uint8_t* p){++published;return p==page;}));
      uint32_t producer=0;assert(BdwCsbReadProducer(page,producer) && producer==5 && published==1);
      for(unsigned i=0x40;i<0x70;++i)assert(page[i]==0xff);
      assert(!BdwCsbInitializeForReset(page,[](uint8_t*){return false;})); }
    { uint32_t engine[1024]{},gt[1024]{};MmioOwner owner;BdwBcsResetMmio<MmioOwner> mmio(owner,engine,gt);
      uint32_t value=0;assert(mmio.writeReg(0x220d0,0x10001) && engine[0xd0/4]==0x10001);
      assert(mmio.writeReg(0x941c,8) && mmio.readReg(0x941c,value) && value==8);
      assert(!mmio.writeReg(0x941c,1) && !mmio.writeReg(0x220d0,0) && !mmio.readReg(0x9000,value));
      owner.held=false;assert(!mmio.writeReg(0x941c,8) && !mmio.readReg(0x220d0,value)); }
    { ResetIo io;assert(BdwResetBcsDomain(io)==BdwBcsResetResult::DomainAcknowledged && io.writes==3 && io.cancels==1); }
    { ResetIo io;io.ctl=2;assert(BdwResetBcsDomain(io)==BdwBcsResetResult::DomainAcknowledged && io.writes==2 && io.cancels==1); }
    { ResetIo io;io.idle=false;assert(BdwResetBcsDomain(io)==BdwBcsResetResult::NotStarted && !io.writes);
      assert(BdwResetBcsDomain(io,BdwBcsResetIntent::StopOwnedWork)==BdwBcsResetResult::DomainAcknowledged && io.stops==1 && io.cancels==1); }
    { ResetIo io;io.idle=false;io.modeIdle=false;assert(BdwResetBcsDomain(io,BdwBcsResetIntent::StopOwnedWork)==BdwBcsResetResult::DomainAcknowledged && io.stops==1 && io.cancels==1); }
    { ResetIo io;io.idle=false;io.failModeRead=true;assert(BdwResetBcsDomain(io,BdwBcsResetIntent::StopOwnedWork)==BdwBcsResetResult::Uncertain && io.stops==1 && io.writes==1); }
    { ResetIo io;io.held=false;assert(BdwResetBcsDomain(io)==BdwBcsResetResult::NotStarted && !io.writes); }
    { ResetIo io;io.ctl=4;assert(BdwResetBcsDomain(io)==BdwBcsResetResult::NotStarted && !io.writes); }
    { ResetIo io;io.domain=1;assert(BdwResetBcsDomain(io)==BdwBcsResetResult::NotStarted && !io.writes); }
    { ResetIo io;io.ready=false;assert(BdwResetBcsDomain(io)==BdwBcsResetResult::Uncertain && io.cancels==1); }
    { ResetIo io;io.complete=false;assert(BdwResetBcsDomain(io)==BdwBcsResetResult::Uncertain && io.cancels==1); }
    { ResetIo io;io.failWrite=true;assert(BdwResetBcsDomain(io)==BdwBcsResetResult::Uncertain); }
    { ResetIo io;io.base=0x2000;io.domainBit=2;
      assert(BdwResetRcsDomain(io)==BdwBcsResetResult::DomainAcknowledged && io.writes==3 && io.cancels==1); }
    { ResetIo io;io.base=0x2000;io.domainBit=2;io.idle=false;
      assert(BdwResetRcsDomain(io)==BdwBcsResetResult::NotStarted && !io.writes);
      assert(BdwResetRcsDomain(io,BdwBcsResetIntent::StopOwnedWork)==BdwBcsResetResult::DomainAcknowledged && io.stops==1); }
    { ResetIo io;io.base=0x2000;io.domainBit=2;io.complete=false;
      assert(BdwResetRcsDomain(io)==BdwBcsResetResult::Uncertain && io.cancels==1); }
    { uint32_t engine[1024]{},gt[1024]{};MmioOwner owner;BdwRcsResetMmio<MmioOwner> mmio(owner,engine,gt);
      uint32_t value=0;assert(mmio.writeReg(0x209c,0x01000100u) && engine[0x9c/4]==0x01000100u);
      assert(mmio.writeReg(0x20d0,0x10001u) && engine[0xd0/4]==0x10001u);
      assert(mmio.writeReg(0x941c,2) && gt[0x41c/4]==2);
      assert(mmio.readReg(0x941c,value) && value==2);
      assert(!mmio.writeReg(0x941c,8) && !mmio.writeReg(0x220d0,0x10001u) && !mmio.readReg(0x220d0,value));
      owner.held=false;assert(!mmio.writeReg(0x941c,2)); }
    { uint32_t regs[1024]{}; regs[0x3a0/4]=7; MmioOwner owner; BdwBcsMmio<MmioOwner> mmio(owner,regs);
      assert(mmio.initializeExeclists(0x10006000));
      assert(regs[0x98/4]==0xffffffffu && regs[0x80/4]==0x10006000);
      assert(!mmio.writeReg(0x22080,0) && !mmio.writeReg(0x22080,0x10006001));
      assert(!mmio.writeReg(0x22000,1) && !mmio.writeReg(0x2229c,0));
      uint32_t value=0; assert(!mmio.readReg(0x22000,value));
      owner.held=false; assert(!mmio.writeReg(0x22098,0xffffffffu) && !mmio.readReg(0x22080,value));
      owner.held=true; owner.synchronized=false; assert(!mmio.initializeExeclists(0x10006000));
      owner.synchronized=true; owner.prepared=false; assert(!mmio.initializeExeclists(0x10006000));
      owner.prepared=true;owner.memoryReady=false;assert(!mmio.initializeExeclists(0x10006000));
      owner.memoryReady=true;owner.producer=4;assert(!mmio.initializeExeclists(0x10006000));
      BdwBcsMmio<MmioOwner> unmapped(owner,nullptr); assert(!unmapped.owned()); }
    { uint32_t regs[1024]{};MmioOwner owner;BdwBcsMmio<MmioOwner> mmio(owner,regs);
      assert(!mmio.synchronizeCsb(0x10006000) && regs[0x3a0/4]==0);
      regs[0x3a0/4]=0xffffffffu;assert(!mmio.synchronizeCsb(0x10006000) && regs[0x3a0/4]==0xffffffffu); }
    { uint32_t regs[1024]{};regs[0x3a0/4]=7;MmioOwner owner;BdwRcsMmio<MmioOwner> mmio(owner,regs);
      assert(mmio.initializeExeclists(0x10006000));
      assert(regs[0x98/4]==0xffffffffu && regs[0x80/4]==0x10006000 && regs[0x3a0/4]==0xffff0505u);
      uint32_t value=0;assert(!mmio.readReg(0x22080,value) && !mmio.writeReg(0x22098,0xffffffffu));
      assert(mmio.prepareSubmission(0x11000013dull));
      assert(mmio.submitDword(0) && mmio.submitDword(0) && mmio.submitDword(1) && mmio.submitDword(0x1000013d));
      assert(regs[0x230/4]==0x1000013d && !mmio.submitDword(0)); }
    { uint32_t regs[1024]{};regs[0x3a0/4]=7;MmioOwner owner;owner.synchronized=false;BdwRcsMmio<MmioOwner> mmio(owner,regs);
      assert(!mmio.initializeExeclists(0x10006000)); }
    uint32_t staging[2048]{};
    {Fake f;BdwBcsOneShot x;f.failGgttWrite=1;assert(x.Prepare(f,65536,99,0x1234,staging));
      assert(x.ScratchNeverPublished() && !x.ExecuteTakenOver(f,0x600000));
      assert(f.slots[0]==0x600001 && !x.ScratchNeverPublished() && x.State()==BdwOneShotState::Prepared);}
    {Fake allocator,backend,wrong;BdwBcsOneShot x;
      assert(x.Prepare(allocator,65536,99,0x1234,staging));
      // The fake backend views the allocator's pinned pages; it does not own
      // or free them. A different allocator cannot authorize submission.
      backend.live=allocator.live;
      assert(!x.ExecuteTakenOverWithBackend(wrong,backend,0x600000) && !backend.ggttWrites);
      BdwBcsExecutionBackend& device=backend;
      assert(x.ExecuteTakenOverWithBackend(allocator,device,0x600000));
      assert(x.PollContextSwitch(wrong)==BdwCsbPoll::Fault && !wrong.csbAcks);
      assert(x.PollContextSwitch(device)==BdwCsbPoll::Pending);
      backend.live.clear();}
    {Fake f;BdwBcsOneShot x;f.losePatOnWrite=8;assert(x.Prepare(f,65536,99,0x1234,staging));
      assert(!x.ExecuteTakenOver(f,0x600000) && !f.engineWrites && !f.portWrites && x.State()==BdwOneShotState::Quarantined);}
    {Fake f;BdwBcsOneShot x;f.losePatOnPort=2;assert(x.Prepare(f,65536,99,0x1234,staging));
      assert(!x.ExecuteTakenOver(f,0x600000) && f.portWrites==2 && x.State()==BdwOneShotState::Quarantined);}
    {Fake f;BdwBcsOneShot x;assert(x.Prepare(f,65536,99,0x1234,staging) && x.Execute(f));
      f.pat=false;assert(x.PollContextSwitch(f)==BdwCsbPoll::Fault && !f.csbAcks && x.State()==BdwOneShotState::Quarantined);}
    {Fake f;BdwBcsOneShot x;f.patLow=f.patHigh=0x03030303;assert(x.Prepare(f,65536,99,0x1234,staging));
      assert(!x.ExecuteTakenOver(f,0x600000) && !f.ggttWrites && !f.engineWrites && x.ReleasePrepared(f));}
    {Fake f;BdwBcsOneShot x;f.changePat=true;assert(x.Prepare(f,65536,99,0x1234,staging));
      assert(!x.ExecuteTakenOver(f,0x600000) && !f.ggttWrites && !f.engineWrites && x.ReleasePrepared(f));}
    {Fake f;BdwBcsOneShot x;f.portReady=false;assert(x.Prepare(f,65536,99,0x1234,staging));
      assert(!x.ExecuteTakenOver(f,0x600000) && !f.portWrites && x.State()==BdwOneShotState::Quarantined);}
    for(unsigned i=1;i<=19;++i){Fake f;f.failAlloc=i;BdwBcsOneShot x;assert(!x.Prepare(f,65536,99,0x1234,staging));assert(f.live.empty() && f.frees==i-1 && !f.engineWrites);}
    for(unsigned kind=1;kind<=2;++kind){Fake f;f.aliasAt=2;f.aliasKind=kind;BdwBcsOneShot x;
        assert(!x.Prepare(f,65536,99,0x1234,staging));assert(x.State()==BdwOneShotState::Quarantined && f.frees==0 && !x.ReleasePrepared(f));}
    {Fake f;BdwBcsOneShot x;f.reentrant=&x;assert(x.Prepare(f,65536,99,0x1234,staging));assert(x.Execute(f));}
    {Fake f;BdwBcsOneShot x;assert(x.Prepare(f,65536,99,0x1234,staging));
        assert(f.live.size()==19);auto* status=f.live[6].cpu;
        for(unsigned i=0;i<4096;++i) {
            const uint8_t expected=(i>=0x40 && i<0x70)?0xff:(i==0x7c?5:0);
            assert(status[i]==expected);
        }
        assert(x.ReleasePrepared(f) && f.live.empty());}
    {Fake f;BdwBcsOneShot x;f.reentrant=&x;assert(x.Prepare(f,65536,99,0x1234,staging));assert(x.ReleasePrepared(f) && f.live.empty());}
    {Fake f;BdwBcsOneShot x;assert(x.Prepare(f,65536,99,0x1234,staging));f.admitted=false;
        assert(!x.Execute(f) && !f.engineWrites && x.ReleasePrepared(f));}
    {Fake f;BdwBcsOneShot x;assert(x.Prepare(f,65536,99,0x1234,staging));f.handoff=false;
        assert(!x.Execute(f) && !f.engineWrites && x.ReleasePrepared(f) && f.live.empty());}
    for(unsigned failure=0;failure<5;++failure){Fake f;BdwBcsOneShot x;assert(x.Prepare(f,65536,99,0x1234,staging));
        if(failure==0)f.execlist=false;else if(failure==1)f.modeReadable=false;else if(failure==2)f.modeStable=false;
        else if(failure==3)f.ringRunning=false;else f.csbReady=false;
        assert(!x.Execute(f) && !f.published && !f.engineWrites && !f.portWrites && !f.ggttWrites && !f.invalidates);
        for(auto slot:f.slots)assert(!slot);
        assert(x.ReleasePrepared(f) && f.live.empty());}
    {Fake f;BdwBcsOneShot x;assert(x.Prepare(f,65536,99,0x1234,staging));f.pat=false;
        assert(!x.Execute(f) && !f.engineWrites && x.ReleasePrepared(f));}
    {Fake f;BdwBcsOneShot x;f.slots[0]=0x12345001;f.slots[7]=0x23456001;
        assert(x.Prepare(f,65536,99,0x1234,staging) && x.Execute(f) && f.ggttWrites==8);}
    {Fake f;BdwBcsOneShot x;f.slots[0]=0xba141f3d556fc1caull;
        assert(x.Prepare(f,65536,99,0x1234,staging) && !x.Execute(f));
        assert(!f.ggttWrites && !f.engineWrites && x.ReleasePrepared(f));}
    {Fake f;BdwBcsOneShot x;f.slots[0]=0xba141f3d556fc1caull;f.takeover=false;
        assert(x.Prepare(f,65536,99,0x1234,staging) && !x.ExecuteTakenOver(f,0x600000));
        assert(!f.ggttWrites && !f.engineWrites && x.ReleasePrepared(f));}
    {Fake f;BdwBcsOneShot x;assert(x.Prepare(f,65536,99,0x1234,staging));
        // Check all 19 allocations, including those outside the GGTT lease.
        for(auto page:f.live)assert(!x.ExecuteTakenOver(f,page.dma));
        assert(!f.ggttWrites && !f.engineWrites && x.ExecuteTakenOver(f,0x600000));}
    {Fake f;BdwBcsOneShot x;f.slots[0]=0xba141f3d556fc1caull;
        f.execlist=false; // firmware left the engine in legacy mode
        assert(x.Prepare(f,65536,99,0x1234,staging) && x.ExecuteTakenOver(f,0x600000));
        assert(f.ggttWrites==8 && f.engineWrites==1 && x.State()==BdwOneShotState::CompleteRetained);}
    {Fake f;BdwBcsOneShot x;f.failHws=true;
        assert(x.Prepare(f,65536,99,0x1234,staging) && !x.ExecuteTakenOver(f,0x600000));
        assert(f.ggttWrites==8 && x.State()==BdwOneShotState::Quarantined && !x.ReleasePrepared(f));}
    {Fake f;BdwBcsOneShot x;f.csbSynced=false;
        assert(x.Prepare(f,65536,99,0x1234,staging) && !x.ExecuteTakenOver(f,0x600000));
        assert(f.ggttWrites==8 && !f.portWrites && x.State()==BdwOneShotState::Quarantined && !x.ReleasePrepared(f));}
    for(unsigned step=1;step<=7;++step){Fake f;BdwBcsOneShot x;f.failInitStep=step;
        assert(x.Prepare(f,65536,99,0x1234,staging) && !x.ExecuteTakenOver(f,0x600000));
        assert(f.ggttWrites==8 && !f.portWrites && x.State()==BdwOneShotState::Quarantined && !x.ReleasePrepared(f));}
    for(unsigned at:{9u,17u}){Fake f;BdwBcsOneShot x;f.alterRead=at;
        assert(x.Prepare(f,65536,99,0x1234,staging) && !x.Execute(f));
        assert(!f.ggttWrites && !f.engineWrites && x.ReleasePrepared(f));}
    for(unsigned i=0;i<=4;++i){Fake f;BdwBcsOneShot x;assert(x.Prepare(f,65536,99,0x1234,staging));
        f.failHws=i==0;f.failPort=i;assert(!x.Execute(f));assert(x.State()==BdwOneShotState::Quarantined && !x.ReleasePrepared(f) && f.live.size()==19);
        // Simulate delayed hardware access after a failed publication.
        f.fromDma(f.slots[6])[0]=0x5a;assert(!x.Execute(f));}
    {Fake f;BdwBcsOneShot x;assert(x.Prepare(f,65536,99,0x1234,staging));f.complete=false;
        assert(!x.Execute(f) && x.State()==BdwOneShotState::Quarantined && f.live.size()==19 && !x.ReleasePrepared(f));}
    {Fake f;BdwBcsOneShot x;assert(x.Prepare(f,65536,99,0x1234,staging));f.corruptSource=true;
        assert(!x.Execute(f) && x.State()==BdwOneShotState::Quarantined && f.live.size()==19);}
    {Fake f;BdwBcsOneShot x;assert(x.Prepare(f,65536,99,0x1234,staging));assert(x.Execute(f));
        assert(x.State()==BdwOneShotState::CompleteRetained && f.live.size()==19 && !x.ReleasePrepared(f));
        f.reentrant=&x;assert(x.PollContextSwitch(f)==BdwCsbPoll::Pending && !f.csbAcks);
        f.tail(0);assert(x.PollContextSwitch(f)==BdwCsbPoll::Pending && !f.csbAcks); // slot still poison
        f.event(0,0xdeadbeef00008001ull);assert(x.PollContextSwitch(f)==BdwCsbPoll::Pending && f.csbAcks==1);
        f.event(1,0x000000010000f818ull);f.tail(1);assert(x.PollContextSwitch(f)==BdwCsbPoll::Complete && f.csbAcks==2);
        assert(f.csbAckSlots[0]==0 && f.csbAckSlots[1]==1 && x.PollContextSwitch(f)==BdwCsbPoll::Complete && f.csbAcks==2);
        assert(x.State()==BdwOneShotState::ContextCompleteRetained && f.live.size()==19 && !x.ReleasePrepared(f));
        f.fromDma(f.slots[6])[0]=0x5a;f.fromDma(f.slots[1])[0]=0x6b;}
    {Fake f;BdwBcsOneShot x;assert(x.Prepare(f,65536,99,0x1234,staging) && x.Execute(f));
        f.event(0,1);f.event(1,0x0000000100000018ull);f.tail(1);
        assert(x.PollContextSwitch(f)==BdwCsbPoll::Complete && f.csbAcks==2 && f.live.size()==19);}
    for(unsigned failure=0;failure<11;++failure){Fake f;BdwBcsOneShot x;
        assert(x.Prepare(f,65536,99,0x1234,staging) && x.Execute(f));
        if(failure==0 || failure==9 || failure==10)f.tail(failure==0?6:failure==9?7:0x81);
        else {f.event(0,1);f.event(1,failure==1?0x0000000200000018ull:failure==2?0x000000010000000aull:
             failure==7?0x0000000100000218ull:failure==8?0x0000000100010018ull:0x0000000100000018ull);f.tail(failure==6?2:1);}
        if(failure==3)f.csbFailAck=true;
        if(failure==4)f.csbFailTail=true;
        if(failure==5)f.csbFailEntry=true;
        assert(x.PollContextSwitch(f)==BdwCsbPoll::Fault && x.State()==BdwOneShotState::Quarantined && f.live.size()==19);}
    {
        BdwBcsOneShot image;
        const BdwBlitRegion region{{BdwBcsSourceAddress,64,64},{BdwBcsScanoutAddress,4096,64},0,0,0,0,16,1};
        const uint64_t source=0x400000;
        BdwPpgttMapping mappings[3]{};
        for(unsigned i=0;i<2;++i){
            Fake f;f.scanout=true;
            assert(!image.PrepareScanout(f,65536,99,0x1234+i,&source,1,0xdd800000,1,&region,1,mappings,2,staging) && !f.allocations);
            assert(image.PrepareScanout(f,65536,99,0x1234+i,&source,1,0xdd800000,1,&region,1,mappings,3,staging));
            assert(f.live.size()==21);
            assert(image.ExecuteTakenOver(f,0x600000) && !f.cpuVisibilityChecks);
            assert(!image.ReleasePrepared(f));
            f.event(0,1);f.event(1,0x0000000100000018ull);f.tail(1);
            assert(image.PollContextSwitch(f)==BdwCsbPoll::Complete && !image.ReleasePrepared(f));
            assert(image.RetireScanout(f));
            for(const auto slot:f.slots)assert(!slot);
            assert(image.ReleasePrepared(f) && f.live.empty() && image.Recycle());
        }
        Fake incompatible;incompatible.scanout=true;incompatible.patLow=0x03030307;
        assert(image.PrepareScanout(incompatible,65536,99,0x1234,&source,1,0xdd800000,1,&region,1,mappings,3,staging));
        assert(!image.ExecuteTakenOver(incompatible,0x600000) && !incompatible.ggttWrites && !incompatible.portWrites);
        assert(image.ReleasePrepared(incompatible));
    }
}
