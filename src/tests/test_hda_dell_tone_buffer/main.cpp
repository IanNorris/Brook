#include <cassert>
#include <cstdint>
#include <cstring>
#include "../../drivers/intel_hda/hda_dell_tone_buffer.h"
using namespace brook;
int main(){
    alignas(4096) uint8_t descriptorPage[4096]{};
    alignas(4096) int16_t data[3][2048]{};
    auto* bdl=reinterpret_cast<HdaToneBdlEntry*>(descriptorPage);
    int16_t* pages[]={data[0],data[1],data[2]};uint64_t phys[]={0x2000,0x3000,0x4000};
    assert(HdaDellBuildToneBuffer(bdl,0x1000,pages,phys,false,2048));
    for(unsigned i=0;i<3;++i){assert(bdl[i].address==phys[i] && bdl[i].length==4096 && !bdl[i].flags);
        for(unsigned f=0;f<1024;++f){int16_t left=data[i][f*2],right=data[i][f*2+1];assert(left==right && left>=-2048 && left<=2048);}}
    assert(data[0][0]==0 && data[0][24]==2048 && data[0][72]==-2048);
    assert(data[2][2046]<0 && data[2][2046]>-2048); // circular end returns to zero
    std::memset(descriptorPage,0x5a,sizeof(descriptorPage));
    const auto originalSample=data[0][24];
    phys[2]=phys[1];assert(!HdaDellBuildToneBuffer(bdl,0x1000,pages,phys,false,2048));
    assert(descriptorPage[0]==0x5a && data[0][24]==originalSample);phys[2]=0x4000;
    assert(!HdaDellBuildToneBuffer(bdl,0x1000,pages,phys,false,2049));
    phys[0]=0x2001;assert(!HdaDellBuildToneBuffer(bdl,0x1000,pages,phys,false,2048));phys[0]=0x2000;
    assert(!HdaDellBuildToneBuffer(bdl,0x2000,pages,phys,false,2048));
    phys[0]=0x100000000ULL;assert(!HdaDellBuildToneBuffer(bdl,0x1000,pages,phys,false,2048));
    assert(HdaDellBuildToneBuffer(bdl,0x1000,pages,phys,true,2048));
    pages[2]=pages[1];assert(!HdaDellBuildToneBuffer(bdl,0x1000,pages,phys,true,2048));
    pages[2]=data[2];
    auto* aliases=reinterpret_cast<uint64_t*>(descriptorPage);
    auto** pointerAliases=reinterpret_cast<int16_t**>(data[0]+64);
    for(unsigned i=0;i<3;++i){aliases[i]=0x2000+i*0x1000;pointerAliases[i]=pages[i];}
    assert(HdaDellBuildToneBuffer(bdl,0x1000,pointerAliases,aliases,false,2048));
    for(unsigned i=0;i<3;++i)assert(bdl[i].address==0x2000+i*0x1000);
}
