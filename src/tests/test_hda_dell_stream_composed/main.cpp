#include <cassert>
#include <cstdint>
#include <cstring>
#include "../../drivers/intel_hda/hda_dell_stream_memory.h"
#include "../../drivers/intel_hda/hda_dell_stream_queue.h"
#include "../../drivers/intel_hda/hda_dell_tone_stream.h"
#include "../test_hda_dell_tone_integration/fake_hda_chip.h"
using namespace brook;
// Composes the production descriptor, memory, publication, queue and stream
// logic against an injected controller. It is not a physical hardware test.
struct Backend:Chip {
    alignas(4096) uint8_t cpu[17][4096]{},device[17][4096]{};
    unsigned allocated=0,freed=0;uint64_t ns=1000000000;bool bme=false,held=false;
    HdaDellStreamMemory<Backend> memory{*this,64};HdaDellToneStream<Backend> stream{*this};
    bool allocatePage(uint64_t& pa,void*& va){if(allocated==17)return false;va=cpu[allocated];pa=0x1000+uint64_t(allocated++)*4096;return true;}
    bool pageAllowed(uint64_t pa,void* va){return pa && va;}
    void clearPage(void* p){std::memset(p,0,4096);}void freePage(uint64_t){assert(!bme);++freed;}
    void line(void* va){auto a=reinterpret_cast<uintptr_t>(va);for(unsigned i=0;i<17;++i){auto b=reinterpret_cast<uintptr_t>(cpu[i]);
        if(a>=b && a<b+4096){assert(!((a-b)&63));std::memcpy(device[i]+a-b,cpu[i]+a-b,64);return;}}assert(false);}
    bool fence(){return true;}
    bool prepare(){pin[0]=pin[1]=0x80;return memory.allocate(false) && stream.prepare(0x4401,memory.bdlPhysical(),0x4011,65536);}
    bool publishDma(){if(!memory.publishAll())return false;published=true;return true;}
    bool write(uint32_t o,const uint8_t* d,uint32_t n){return memory.write(o,d,n);}
    bool publish(uint32_t o,uint32_t n){return memory.publish(o,n);}
    uint64_t nowNs(){ns+=1000000;return ns;}
    uint32_t read32(uint32_t o){if(o==base+4 && (read16(base)&2)){pos=(pos+192)%65536;return pos;}return Chip::read32(o);}
    bool position(uint32_t& raw,bool& error,bool& running){raw=read32(base+4);error=(read8(base+3)&0x18)!=0;running=(read16(base)&2)!=0;return true;}
    bool start(){if(!memory.publishAll() || !memory.expose())return false;
        assert(reinterpret_cast<const HdaToneBdlEntry*>(device[0])[0].address==0x2000);
        bme=true;return stream.start();}
    bool mute(){pin[0]=pin[1]=0x80;return true;}
    bool stop(){if(!stream.stop())return false;bme=false;return true;}
    void quarantine(){held=true;memory.quarantine();}
    bool retire(){return !bme && stream.retire() && memory.release(true);}
};
int main(){
    Backend p;assert(p.prepare());HdaDellStreamQueue<Backend> q(p,64,4096);assert(q.start());
    auto* bdl=reinterpret_cast<const HdaToneBdlEntry*>(p.device[0]);
    for(unsigned i=0;i<16;++i)assert(bdl[i].address==0x2000+uint64_t(i)*4096 && bdl[i].length==4096);
    uint8_t samples[128];std::memset(samples,0x37,sizeof(samples));auto receipt=q.submit(samples,sizeof(samples));
    assert(!receipt.error && receipt.accepted==sizeof(samples));
    for(unsigned i=0;i<sizeof(samples);++i){const auto o=(receipt.begin+i)%65536;assert(p.device[1+o/4096][o%4096]==0x37);}
    for(unsigned i=0;i<900;++i)assert(q.service());
    assert(q.progress().RawCompleted()>2*65536 && !q.failed());
    for(unsigned i=0;i<sizeof(samples);++i){const auto o=(receipt.begin+i)%65536;assert(p.device[1+o/4096][o%4096]==0);}
    assert(q.shutdown() && !p.bme && p.retire() && p.freed==17 && !p.held);
    Backend broken;assert(broken.prepare());HdaDellStreamQueue<Backend> bad(broken,64,4096);assert(bad.start());
    broken.dropStop=true;assert(!bad.shutdown() && broken.held && !broken.memory.release(true) && broken.freed==0);
}
