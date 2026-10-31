#include "../../kernel/src/memory/kernel_alias_probe.h"
#include <cassert>
#include <cstdint>
using namespace brook;
struct Tables {
    uint64_t data[4][512]{};unsigned reads=0;
    bool readEntry(uint64_t page,uint16_t index,uint64_t& out){
        ++reads;if(page<0x1000 || page>0x4000 || (page&4095) || index>=512)return false;
        out=data[page/4096-1][index];return true;
    }
};
int main(){
    Tables t;KernelAliasMapping out{};constexpr uint64_t va=0x12345000;
    t.data[0][0]=0x2003;t.data[1][0]=0x3003;t.data[2][(va>>21)&511]=0x4003;
    const unsigned pt=(va>>12)&511;
    for(unsigned pat=0;pat<8;++pat){
        uint64_t flags=3|uint64_t(pat&1)<<3|uint64_t((pat>>1)&1)<<4|uint64_t((pat>>2)&1)<<7;
        t.data[3][pt]=0x56789000|flags;
        assert(InspectKernelAlias(t,0x1000,va,out) && out.present && out.pageShift==12 && out.patIndex==pat && out.physical==0x56789000);
    }
    for(unsigned pat=0;pat<8;++pat){
        uint64_t flags=0x83|uint64_t(pat&1)<<3|uint64_t((pat>>1)&1)<<4|uint64_t((pat>>2)&1)<<12;
        t.data[2][(va>>21)&511]=0x40000000|flags;
        assert(InspectKernelAlias(t,0x1000,va,out) && out.present && out.pageShift==21 && out.patIndex==pat && out.physical==0x40000000+(va&0x1fffff));
        t.data[1][0]=0x80000000|flags;
        assert(InspectKernelAlias(t,0x1000,va,out) && out.present && out.pageShift==30 && out.patIndex==pat && out.physical==0x80000000+va);
        t.data[1][0]=0x3003;
    }
    t.data[0][256]=0x2003;t.data[2][(va>>21)&511]=0x4003;t.data[3][pt]=0x56789003;
    assert(InspectKernelAlias(t,0x1000,0xffff800012345000ull,out) && out.present && out.physical==0x56789000);
    t.data[2][(va>>21)&511]=0;assert(InspectKernelAlias(t,0x1000,va,out) && !out.present);
    t.data[2][(va>>21)&511]=0x40002083;assert(!InspectKernelAlias(t,0x1000,va,out));
    t.data[1][0]=0x5003;assert(!InspectKernelAlias(t,0x1000,va,out));
    t.data[1][0]=3;assert(!InspectKernelAlias(t,0x1000,va,out));
    t.data[0][0]=0x2083;assert(!InspectKernelAlias(t,0x1000,va,out));
    assert(!InspectKernelAlias(t,0x1001,va,out) && !InspectKernelAlias(t,1ull<<34,va,out) &&
           !InspectKernelAlias(t,0x1000,0x0000800000000000ull,out));
}
