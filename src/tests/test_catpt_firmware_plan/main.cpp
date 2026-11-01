#include "catpt_firmware_plan.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    std::ifstream f(argv[1],std::ios::binary|std::ios::ate);
    if(!f)return 2;
    auto length=f.tellg();
    if(length<0)return 2;
    if(length>16*1024*1024){std::puts("invalid");return 1;}
    f.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(length));
    if(length && !f.read(reinterpret_cast<char*>(data.data()),length))return 2;
    char extra;if(f.get(extra) || !f.eof())return 2;
    brook::CatptFirmwarePlan p{};
    auto* raw=reinterpret_cast<unsigned char*>(&p);
    for(size_t i=0;i<sizeof(p);++i)raw[i]=0xa5;
    unsigned char before[sizeof(p)];std::memcpy(before,&p,sizeof(p));
    if(!brook::CatptBuildPlan(data.data(),static_cast<uint32_t>(data.size()),p)) {
        if(std::memcmp(&p,before,sizeof(p)))return 3;
        std::puts("invalid");return 1;
    }
    // Mock copies are performed only after the whole plan was validated.
    std::vector<uint8_t> iram(0x50000,0xa5),dram(0xa0000,0xa5);
    std::vector<bool> iramTouched(iram.size()),dramTouched(dram.size());
    for(uint32_t i=0;i<p.modules;++i) {
        const auto& m=p.module[i];
        std::printf("M %u %u %u %u %u %u %u\n",m.id,m.slot,m.entry,m.persistent,m.scratch,m.firstBlock,m.blocks);
    }
    for(uint32_t i=0;i<p.blocks;++i) {
        const auto& b=p.block[i];auto& target=b.region==brook::CatptRegion::Iram?iram:dram;
        auto& touched=b.region==brook::CatptRegion::Iram?iramTouched:dramTouched;
        for(uint32_t j=0;j<b.size;++j){target[b.offset+j]=data[b.source+j];touched[b.offset+j]=true;}
        std::printf("B %u %u %u %u\n",unsigned(b.region),b.source,b.size,b.offset);
    }
    for(uint32_t i=0;i<p.blocks;++i) {
        const auto& b=p.block[i];const auto& target=b.region==brook::CatptRegion::Iram?iram:dram;
        for(uint32_t j=0;j<b.size;++j)if(target[b.offset+j]!=data[b.source+j])return 4;
    }
    for(size_t i=0;i<iram.size();++i)if(!iramTouched[i] && iram[i]!=0xa5)return 4;
    for(size_t i=0;i<dram.size();++i)if(!dramTouched[i] && dram[i]!=0xa5)return 4;
    std::fprintf(stderr,"modules=%u blocks=%u\n",p.modules,p.blocks);
    return 0;
}
