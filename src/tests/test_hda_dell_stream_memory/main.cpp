#include <cassert>
#include <cstdint>
#include <cstring>
#include "../../drivers/intel_hda/hda_dell_stream_memory.h"
using namespace brook;
struct Platform {
    alignas(4096) uint8_t cpu[17][4096]{},device[17][4096]{};
    unsigned count=0,freed=0,lines=0,fences=0,cleared=0;bool freedPage[17]{};int failAt=-1,duplicateAt=-1,denyAt=-1;
    bool allocatePage(uint64_t& pa,void*& va){if(int(count)==failAt)return false;
        assert(count<17);va=cpu[count];pa=0x1000+uint64_t(count)*4096;
        if(int(count)==duplicateAt)pa=0x1000;++count;return true;}
    bool pageAllowed(uint64_t pa,void* va){return pa && va && int(count-1)!=denyAt;}
    void clearPage(void* va){++cleared;std::memset(va,0,4096);}
    void freePage(uint64_t pa){assert(pa>=0x1000 && pa<=0x11000 && !(pa&4095));const auto i=unsigned(pa/4096-1);
        assert(!freedPage[i]);freedPage[i]=true;++freed;}
    void line(void* va){for(unsigned i=0;i<17;++i){auto base=reinterpret_cast<uintptr_t>(cpu[i]);auto p=reinterpret_cast<uintptr_t>(va);
        if(p>=base && p<base+4096){assert(!((p-base)&63));std::memcpy(device[i]+(p-base),cpu[i]+(p-base),64);++lines;return;}}assert(false);}
    bool fence(){++fences;return true;}
};
int main(){
    Platform p;HdaDellStreamMemory<Platform> m(p,64);assert(m.allocate(false) && m.bdlPhysical()==0x1000);
    assert(m.publishAll() && p.lines==17*64 && p.fences==1);
    auto* bdl=reinterpret_cast<const HdaToneBdlEntry*>(p.device[0]);
    for(unsigned i=0;i<16;++i)assert(bdl[i].address==0x2000+uint64_t(i)*4096 && bdl[i].length==4096 && !bdl[i].flags);
    const uint8_t sample[]={1,2,3,4,5,6,7,8};assert(m.write(4092,sample,8));
    assert(!p.device[1][4092] && m.publish(4092,8) && m.fence());
    assert(p.device[1][4092]==1 && p.device[1][4095]==4 && p.device[2][0]==5 && p.device[2][3]==8);
    assert(m.write(4092,nullptr,8) && m.publish(4092,8) && m.fence());
    assert(!p.device[1][4092] && !p.device[2][3]);
    assert(!m.write(65535,sample,2) && !m.publish(65535,2));
    assert(m.expose() && !m.release(false) && p.freed==0 && m.release(true) && p.freed==17 && !m.allocate(false));
    Platform failed;failed.failAt=9;HdaDellStreamMemory<Platform> a(failed,64);
    assert(!a.allocate(false) && failed.freed==9);
    Platform duplicate;duplicate.duplicateAt=3;HdaDellStreamMemory<Platform> b(duplicate,64);
    assert(!b.allocate(false) && duplicate.freed==16);
    Platform denied;denied.denyAt=4;HdaDellStreamMemory<Platform> d(denied,64);
    assert(!d.allocate(false) && denied.freed==5 && denied.cleared==4);
    Platform retained;HdaDellStreamMemory<Platform> c(retained,64);assert(c.allocate(false) && c.expose());
    c.quarantine();assert(!c.release(true) && retained.freed==0 && !c.write(0,sample,8));
}
