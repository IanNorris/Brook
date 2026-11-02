#include <cassert>
#include <cstdint>
#include <cstring>
#include "../../drivers/intel_hda/hda_dell_stream_bdl.h"
using namespace brook;
int main(){
    alignas(4096) uint8_t descriptor[4096]{},data[16][4096]{};
    auto* bdl=reinterpret_cast<HdaToneBdlEntry*>(descriptor);void* pages[16];uint64_t phys[16];
    for(unsigned i=0;i<16;++i){pages[i]=data[i];phys[i]=0x2000+uint64_t(i)*4096;}
    assert(HdaDellBuildStreamBdl(bdl,0x1000,pages,phys,false));
    for(unsigned i=0;i<16;++i)assert(bdl[i].address==phys[i] && bdl[i].length==4096 && !bdl[i].flags);
    std::memset(descriptor,0x5a,sizeof(descriptor));
    const auto checkUnchanged=[&](){for(auto c:descriptor)assert(c==0x5a);};
    phys[15]=phys[0];assert(!HdaDellBuildStreamBdl(bdl,0x1000,pages,phys,false));checkUnchanged();phys[15]=0x11000;
    pages[15]=pages[0];assert(!HdaDellBuildStreamBdl(bdl,0x1000,pages,phys,false));checkUnchanged();pages[15]=data[15];
    phys[3]=0x100000000ULL;assert(!HdaDellBuildStreamBdl(bdl,0x1000,pages,phys,false));checkUnchanged();
    assert(HdaDellBuildStreamBdl(bdl,0x1000,pages,phys,true) && bdl[3].address==0x100000000ULL);
    phys[3]=0x1000;assert(!HdaDellBuildStreamBdl(bdl,0x1000,pages,phys,true));
}
