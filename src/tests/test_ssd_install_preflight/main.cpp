#include "../../kernel/src/ssd_install_preflight.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <vector>
using namespace brook;
static void put32(uint8_t* p,uint32_t v){for(int i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
static void put64(uint8_t* p,uint64_t v){put32(p,uint32_t(v));put32(p+4,uint32_t(v>>32));}
static void header(uint8_t* h,bool back,uint32_t crc){std::memset(h,0,512);std::memcpy(h,"EFI PART",8);
    put32(h+8,0x10000);put32(h+12,92);put64(h+24,back?99999:1);put64(h+32,back?1:99999);
    put64(h+40,34);put64(h+48,99966);h[56]=1;put64(h+72,back?99967:2);
    put32(h+80,128);put32(h+84,128);put32(h+88,crc);put32(h+16,StrictCrc32(h,92,true));}
static int fixture(const char* path){
    std::ifstream f(path,std::ios::binary);if(!f)return 1;f.seekg(0,std::ios::end);const auto size=f.tellg();
    if(size<0 || uint64_t(size)%512)return 1;const uint64_t sectors=uint64_t(size)/512;if(sectors<68)return 1;
    uint8_t mbr[512],primary[512],backup[512],entries[16384],copy[16384];
    f.seekg(0);f.read(reinterpret_cast<char*>(mbr),512);f.read(reinterpret_cast<char*>(primary),512);
    f.seekg(2*512);f.read(reinterpret_cast<char*>(entries),sizeof(entries));
    f.seekg((sectors-33)*512);f.read(reinterpret_cast<char*>(copy),sizeof(copy));f.read(reinterpret_cast<char*>(backup),512);if(!f)return 1;
    const auto first=StrictLe64(entries+32),last=StrictLe64(entries+40);if(last<first)return 1;
    return SsdInstallPreflight({sectors,first,last-first+1},sectors,sectors,512,mbr,primary,backup,entries,copy,sizeof(entries))?0:1;
}
int main(int argc,char** argv){
    if(argc==2)return fixture(argv[1]);assert(argc==1);
    uint8_t mbr[512]{},primary[512]{},backup[512]{},entries[16384]{},copy[16384]{};
    mbr[510]=0x55;mbr[511]=0xaa;mbr[450]=0xee;put32(mbr+454,1);put32(mbr+458,99999);
    const uint8_t esp[16]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0,0xa0,0xc9,0x3e,0xc9,0x3b};
    std::memcpy(entries,esp,16);entries[16]=2;put64(entries+32,2048);put64(entries+40,4095);
    std::memcpy(copy,entries,sizeof(entries));const auto crc=StrictCrc32(entries,sizeof(entries));header(primary,false,crc);header(backup,true,crc);
    const SsdInstallGeometry g{100000,2048,2048};
    auto valid=[&](){return SsdInstallPreflight(g,100000,100000,512,mbr,primary,backup,entries,copy,sizeof(entries));};
    assert(valid());assert(!SsdInstallPreflight(g,100000,99999,512,mbr,primary,backup,entries,copy,sizeof(entries)));
    assert(!SsdInstallPreflight(g,99999,100000,512,mbr,primary,backup,entries,copy,sizeof(entries)));
    assert(!SsdInstallPreflight(g,100000,100000,4096,mbr,primary,backup,entries,copy,sizeof(entries)));
    assert(!SsdInstallPreflight(g,100000,100000,512,mbr,primary,backup,entries,copy,sizeof(entries)-1));
    assert(!SsdInstallPreflight({100000,2048,2049},100000,100000,512,mbr,primary,backup,entries,copy,sizeof(entries)));
    copy[42]^=1;assert(!valid());copy[42]^=1;mbr[466]=0x83;assert(!valid());mbr[466]=0;
    put64(primary+72,3);put32(primary+16,0);put32(primary+16,StrictCrc32(primary,92,true));assert(!valid());header(primary,false,crc);
    std::memcpy(entries+10*128,entries,128);std::memset(entries,0,128);std::memcpy(copy,entries,sizeof(entries));
    const auto relocatedCrc=StrictCrc32(entries,sizeof(entries));header(primary,false,relocatedCrc);header(backup,true,relocatedCrc);assert(valid());
    std::memset(entries+10*128,0,128);std::memcpy(copy,entries,sizeof(entries));
    const auto emptyCrc=StrictCrc32(entries,sizeof(entries));header(primary,false,emptyCrc);header(backup,true,emptyCrc);assert(!valid());
    std::memcpy(entries,esp,16);entries[16]=2;put64(entries+32,2048);put64(entries+40,4095);
    std::memcpy(copy,entries,sizeof(entries));header(primary,false,crc);header(backup,true,crc);assert(valid());
    entries[0]^=1;std::memcpy(copy,entries,sizeof(entries));const auto wrongCrc=StrictCrc32(entries,sizeof(entries));
    header(primary,false,wrongCrc);header(backup,true,wrongCrc);assert(!valid());
    entries[0]^=1;std::memcpy(entries+128,esp,16);entries[128+16]=3;put64(entries+128+32,5000);put64(entries+128+40,6000);
    std::memcpy(copy,entries,sizeof(entries));const auto extraCrc=StrictCrc32(entries,sizeof(entries));
    header(primary,false,extraCrc);header(backup,true,extraCrc);assert(!valid());
}
