#include "../../kernel/src/ahci_boot_selector.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using namespace brook;
static void put32(uint8_t* p,uint32_t v){for(int i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
static void put64(uint8_t* p,uint64_t v){put32(p,uint32_t(v));put32(p+4,uint32_t(v>>32));}
static void header(uint8_t* h,bool backup,uint32_t crc){
    std::memset(h,0,512);std::memcpy(h,"EFI PART",8);put32(h+8,0x10000);put32(h+12,92);
    put64(h+24,backup?99999:1);put64(h+32,backup?1:99999);put64(h+40,34);put64(h+48,99966);
    h[56]=1;put64(h+72,backup?99967:2);put32(h+80,128);put32(h+84,128);put32(h+88,crc);put32(h+16,StrictCrc32(h,92,true));
}
static int fixture(const char* path,const char* recordPath) {
    std::ifstream f(path,std::ios::binary),record(recordPath,std::ios::binary);if(!f || !record)return 1;
    std::string text{std::istreambuf_iterator<char>(record),std::istreambuf_iterator<char>()};
    AhciBootSelector want{};if(!AhciBootSelectorParse(text.data(),text.size(),want))return 1;
    f.seekg(0,std::ios::end);const auto size=f.tellg();if(size<0 || uint64_t(size)%512)return 1;
    const uint64_t sectors=uint64_t(size)/512;uint8_t mbr[512],primary[512],backup[512];
    f.seekg(0);f.read(reinterpret_cast<char*>(mbr),512);f.read(reinterpret_cast<char*>(primary),512);
    if(sectors<68)return 1;f.seekg((sectors-1)*512);f.read(reinterpret_cast<char*>(backup),512);if(!f)return 1;
    StrictGptHeader a{},b{};
    if(StrictGptCheckHeader(primary,512,sectors,false,a)!=StrictGptResult::Ok ||
       StrictGptCheckHeader(backup,512,sectors,true,b)!=StrictGptResult::Ok)return 1;
    const uint32_t bytes=a.count*a.entrySize;std::vector<uint8_t> entries(bytes),copy(bytes);
    f.seekg(a.entriesLba*512);f.read(reinterpret_cast<char*>(entries.data()),bytes);
    f.seekg(b.entriesLba*512);f.read(reinterpret_cast<char*>(copy.data()),bytes);if(!f)return 1;
    AhciBootSelection result{};
    return AhciBootSelect(want,sectors,mbr,primary,backup,entries.data(),copy.data(),bytes,result)?0:1;
}
int main(int argc,char** argv){
    if(argc==3)return fixture(argv[1],argv[2]);
    assert(argc==1);
    const std::string text="BRKESP1\n01000000000000000000000000000000\n02000000000000000000000000000000\n2048,2048\n";
    AhciBootSelector want{};assert(AhciBootSelectorParse(text.data(),text.size(),want));
    auto bad=[&](const std::string& s){AhciBootSelector x=want;assert(!AhciBootSelectorParse(s.data(),s.size(),x));assert(!x.first && !x.diskGuid[0]);};
    bad(text+"x");bad(text.substr(0,text.size()-1));bad("BRKESP1\n00000000000000000000000000000000\n02000000000000000000000000000000\n2048,2048\n");
    bad("BRKESP1\n0100000000000000000000000000000X\n02000000000000000000000000000000\n2048,2048\n");
    bad("BRKESP1\n01000000000000000000000000000000\n02000000000000000000000000000000\n18446744073709551615,2\n");
    uint8_t mbr[512]{},primary[512],backup[512],entries[16384]{},copy[16384]{};
    mbr[510]=0x55;mbr[511]=0xaa;mbr[450]=0xee;put32(mbr+454,1);put32(mbr+458,99999);
    const uint8_t esp[16]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0,0xa0,0xc9,0x3e,0xc9,0x3b};
    std::memcpy(entries+128,esp,16);entries[128+16]=2;put64(entries+128+32,2048);put64(entries+128+40,4095);
    std::memcpy(copy,entries,sizeof(entries));const auto crc=StrictCrc32(entries,sizeof(entries));header(primary,false,crc);header(backup,true,crc);
    AhciBootSelection selected{};assert(AhciBootSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));
    assert(selected.entry==2 && selected.first==2048 && selected.count==2048);
    ++want.count;assert(!AhciBootSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));--want.count;
    ++want.first;assert(!AhciBootSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));--want.first;
    want.partitionGuid[0]^=1;assert(!AhciBootSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));want.partitionGuid[0]^=1;
    want.diskGuid[0]^=1;assert(!AhciBootSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));want.diskGuid[0]^=1;
    assert(!AhciBootSelect(want,100000,mbr,primary,backup,entries,copy,0,selected));
    assert(!AhciBootSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries)-1,selected));
    copy[100]^=1;assert(!AhciBootSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));copy[100]^=1;
    entries[128]^=1;std::memcpy(copy,entries,sizeof(entries));const auto wrongTypeCrc=StrictCrc32(entries,sizeof(entries));
    header(primary,false,wrongTypeCrc);header(backup,true,wrongTypeCrc);
    assert(!AhciBootSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));
    entries[128]^=1;std::memcpy(copy,entries,sizeof(entries));header(primary,false,crc);header(backup,true,crc);
    std::memcpy(entries+256,esp,16);entries[256+16]=2;put64(entries+256+32,5000);put64(entries+256+40,6000);
    std::memcpy(copy,entries,sizeof(entries));const auto duplicateCrc=StrictCrc32(entries,sizeof(entries));
    header(primary,false,duplicateCrc);header(backup,true,duplicateCrc);
    assert(!AhciBootSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));
    std::memset(entries+256,0,128);std::memcpy(copy,entries,sizeof(entries));header(primary,false,crc);header(backup,true,crc);
    mbr[466]=0x83;assert(!AhciBootSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));
}
