#include "intel_bdw_oneshot.h"
#include "intel_bdw_csb.h"
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
using namespace brook;
struct Fake {
    std::vector<BdwOwnedPage> live;uint64_t slots[8]{};uint32_t elsp[4]{};
    unsigned allocations=0,frees=0,engineWrites=0,portWrites=0,ggttWrites=0,invalidates=0,failAlloc=0,failPort=0;
    unsigned csbAcks=0,csbAckSlots[6]{};bool csbFailAck=false,csbFailTail=false,csbFailEntry=false;
    bool handoff=true,execlist=true,modeReadable=true,modeStable=true,ringRunning=true,csbReady=true;
    bool admitted=true,pat=true,published=false,failHws=false,complete=true,idle=true,corruptSource=false;
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
    bool gpuPatVerified(){return pat;}
    bool admitPages(BdwOwnedPage*,unsigned n){assert(n==19);return admitted;}
    bool publish(BdwOwnedPage*,unsigned n){assert(n==19);if(reentrant)assert(!reentrant->ReleasePrepared(*this));published=true;return true;}
    bool validateLease(const BdwGgttLease& l){return handoff && l.token==99 && l.count==8;}
    bool quiescent(){return idle;}
    bool read(uint32_t i,uint64_t& v){assert(i>=65536 && i<65544);v=slots[i-65536];return true;}
    bool write(uint32_t i,uint64_t v){assert(i>=65536 && i<65544);++ggttWrites;slots[i-65536]=v;return true;}
    bool invalidate(){++invalidates;return true;}
    bool programHws(uint64_t addr){++engineWrites;assert(published && addr==0x10006000);return !failHws;}
    bool submitDword(uint32_t v){assert(portWrites<4);elsp[portWrites++]=v;return portWrites!=failPort;}
    uint8_t* fromDma(uint64_t addr){addr&=~uint64_t(4095);for(auto p:live)if(p.dma==addr)return p.cpu;assert(false);return nullptr;}
    uint64_t read64(uint8_t* p,unsigned index){uint64_t v=0;for(unsigned i=0;i<8;++i)v|=uint64_t(p[index*8+i])<<(8*i);return v;}
    uint32_t read32(uint8_t* p,unsigned index){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(p[index*4+i])<<(8*i);return v;}
    uint8_t* ggtt(uint32_t address){unsigned slot=(address>>12)-65536;assert(slot<8);return fromDma(slots[slot])+(address&4095);}
    uint8_t* translate(uint64_t root,uint64_t address,bool writable){
        auto* p=fromDma(root);for(unsigned shift:{39,30,21}){uint64_t e=read64(p,(address>>shift)&511);assert((e&3)==3);p=fromDma(e);}
        uint64_t e=read64(p,(address>>12)&511);assert((e&0x81)==0x81 && bool(e&2)==writable);return fromDma(e)+(address&4095);}
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
        for(unsigned row=0;row<height;++row)
            std::memcpy(translate(pml4,dst+row*(read32(batch,1)&0xffff),true),translate(pml4,src+row*read32(batch,7),false),width);
        auto* done=ggtt(read32(ring,9)&~7u);uint32_t written=read32(ring,11);assert(written==nonce);std::memcpy(done,&written,4);return true;}
    bool cpuVisible(BdwOwnedPage*,unsigned n){return n==19;}
    bool csbReadTail(BdwOwnedPage& hws,uint32_t& tail){if(reentrant)assert(reentrant->PollContextSwitch(*this)==BdwCsbPoll::Fault);
        return !csbFailTail && BdwCsbReadProducer(hws.cpu,tail);}
    bool csbReadEntry(BdwOwnedPage& hws,unsigned i,uint64_t& event){return !csbFailEntry && BdwCsbReadRecord(hws.cpu,i,event);}
    bool csbAcknowledge(unsigned slot){assert(csbAcks<6);csbAckSlots[csbAcks++]=slot;return !csbFailAck;}
    void event(unsigned index,uint64_t value){assert(index<6 && live.size()==19);std::memcpy(live[6].cpu+0x40+8*index,&value,8);}
    void tail(uint32_t value){assert(live.size()==19);std::memcpy(live[6].cpu+0x7c,&value,4);}
};
int main(){
    uint32_t staging[2048]{};
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
}
