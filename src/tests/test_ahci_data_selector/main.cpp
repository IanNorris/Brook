#include "ahci_data_selector.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
using namespace brook;
static void put32(uint8_t* p,uint32_t v){for(int i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
static void put64(uint8_t* p,uint64_t v){put32(p,uint32_t(v));put32(p+4,uint32_t(v>>32));}
static void header(uint8_t* h,bool backup,uint32_t crc){
    std::memset(h,0,512);std::memcpy(h,"EFI PART",8);put32(h+8,0x10000);put32(h+12,92);
    put64(h+24,backup?99999:1);put64(h+32,backup?1:99999);put64(h+40,34);put64(h+48,99966);
    h[56]=1;put64(h+72,backup?99967:2);put32(h+80,128);put32(h+84,128);
    put32(h+88,crc);put32(h+16,StrictCrc32(h,92,true));
}
static int fixture(const char* path) {
    std::ifstream f(path,std::ios::binary);if(!f)return 1;
    f.seekg(0,std::ios::end);auto length=f.tellg();if(length<0 || uint64_t(length)%512)return 1;
    const uint64_t sectors=uint64_t(length)/512;if(sectors<68)return 1;
    uint8_t mbr[512]{},primary[512]{},backup[512]{};
    f.seekg(0);f.read(reinterpret_cast<char*>(mbr),512);f.read(reinterpret_cast<char*>(primary),512);
    f.seekg((sectors-1)*512);f.read(reinterpret_cast<char*>(backup),512);if(!f)return 1;
    StrictGptHeader a{},b{};
    if(StrictGptCheckHeader(primary,512,sectors,false,a)!=StrictGptResult::Ok ||
       StrictGptCheckHeader(backup,512,sectors,true,b)!=StrictGptResult::Ok)return 1;
    const uint32_t bytes=a.count*a.entrySize;std::vector<uint8_t> entries(bytes),copy(bytes);
    f.seekg(a.entriesLba*512);f.read(reinterpret_cast<char*>(entries.data()),bytes);
    f.seekg(b.entriesLba*512);f.read(reinterpret_cast<char*>(copy.data()),bytes);if(!f)return 1;
    AhciDataSelector want{};std::memcpy(want.diskGuid,a.diskGuid,16);
    std::memcpy(want.partitionGuid,entries.data()+a.entrySize+16,16);
    want.filesystemUuid[0]=1;want.first=16384;want.count=32768;
    AhciBootSelection selected{};
    return AhciDataSelect(want,sectors,mbr,primary,backup,entries.data(),copy.data(),bytes,selected) &&
           selected.entry==2 && selected.first==16384 && selected.count==32768 ? 0 : 1;
}
int main(int argc,char** argv){
    if(argc==2)return fixture(argv[1]);
    assert(argc==1);
    const std::string text="BRKDATA1\n01000000000000000000000000000000\n"
        "02000000000000000000000000000000\n2048,2048\n03000000000000000000000000000000\n";
    AhciDataSelector want{};assert(AhciDataSelectorParse(text.data(),text.size(),want));
    auto bad=[&](const std::string& s){AhciDataSelector x=want;assert(!AhciDataSelectorParse(s.data(),s.size(),x));assert(!x.first && !x.diskGuid[0]);};
    bad(text+"x");bad(text.substr(0,text.size()-1));
    bad("BRKDATA1\n01000000000000000000000000000000\n02000000000000000000000000000000\n2048,2048\n00000000000000000000000000000000\n");
    bad("BRKDATA1\n01000000000000000000000000000000\n02000000000000000000000000000000\n18446744073709551615,2\n03000000000000000000000000000000\n");
    bad("BRKDATA1\n01000000000000000000000000000000\n02000000000000000000000000000000\n0,1\n03000000000000000000000000000000\n");
    bad("BRKDATA1\n01000000000000000000000000000000\n02000000000000000000000000000000\n1,36028797018963968\n03000000000000000000000000000000\n");
    uint8_t mbr[512]{},primary[512]{},backup[512]{},entries[16384]{},copy[16384]{};
    mbr[510]=0x55;mbr[511]=0xaa;mbr[450]=0xee;put32(mbr+454,1);put32(mbr+458,99999);
    const uint8_t linuxFs[16]={0x0f,0xc6,0x3d,0xaf,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
    std::memcpy(entries+128,linuxFs,16);entries[144]=2;put64(entries+160,2048);put64(entries+168,4095);
    auto refresh=[&](){std::memcpy(copy,entries,sizeof(entries));auto crc=StrictCrc32(entries,sizeof(entries));header(primary,false,crc);header(backup,true,crc);};
    refresh();AhciBootSelection selected{};
    assert(AhciDataSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));
    assert(selected.entry==2 && selected.first==2048 && selected.count==2048);
    assert(!AhciDataSelect(want,~uint64_t{0},mbr,primary,backup,entries,copy,sizeof(entries),selected) && !selected.entry && !selected.count);
    assert(!AhciDataSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries)-1,selected));
    want.filesystemUuid[0]=0;assert(!AhciDataSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));want.filesystemUuid[0]=3;
    ++want.count;assert(!AhciDataSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));--want.count;
    want.diskGuid[0]^=1;assert(!AhciDataSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));want.diskGuid[0]^=1;
    want.partitionGuid[0]^=1;assert(!AhciDataSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));want.partitionGuid[0]^=1;
    copy[100]^=1;assert(!AhciDataSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));copy[100]^=1;
    entries[128]^=1;refresh();assert(!AhciDataSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));
    entries[128]^=1;refresh();
    std::memcpy(entries+256,linuxFs,16);entries[272]=2;put64(entries+288,5000);put64(entries+296,6000);
    refresh();assert(!AhciDataSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));
    entries[272]=4;put64(entries+288,3000);refresh();
    assert(!AhciDataSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));
    std::memset(entries+256,0,128);refresh();mbr[466]=0x83;
    assert(!AhciDataSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));
}
