#include "ahci_data_reader.h"
#include "ahci_direct_layout.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
using namespace brook;
static void put32(uint8_t* p,uint32_t v){for(int i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
static void put16(uint8_t* p,uint16_t v){p[0]=uint8_t(v);p[1]=uint8_t(v>>8);}
static void put64(uint8_t* p,uint64_t v){put32(p,uint32_t(v));put32(p+4,uint32_t(v>>32));}
static void header(uint8_t* h,bool backup,uint32_t crc){
    std::memset(h,0,512);std::memcpy(h,"EFI PART",8);put32(h+8,0x10000);put32(h+12,92);
    put64(h+24,backup?99999:1);put64(h+32,backup?1:99999);put64(h+40,34);put64(h+48,99966);
    h[56]=1;put64(h+72,backup?99967:2);put32(h+80,128);put32(h+84,128);
    put32(h+88,crc);put32(h+16,StrictCrc32(h,92,true));
}
static int fixture(const char* path,const char* storePath,uint64_t dataCount) {
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
    want.filesystemUuid[0]=1;want.first=16384;want.count=dataCount;
    AhciBootSelector boot{};std::memcpy(boot.diskGuid,a.diskGuid,16);
    std::memcpy(boot.partitionGuid,entries.data()+16,16);boot.first=2048;boot.count=8192;
    AhciBootSelection selected{}, selectedBoot{};
    if(!AhciSelectBootAndData(boot,want,sectors,mbr,primary,backup,entries.data(),copy.data(),bytes,selectedBoot,selected) ||
       selectedBoot.entry!=1 || selected.entry!=2 || selected.first!=16384 || selected.count!=dataCount)return 1;
    AhciDirectLayout direct{};
    if(!AhciDirectLayoutSelect(boot,want,sectors,mbr,primary,backup,entries.data(),copy.data(),bytes,direct) ||
       direct.esp.entry!=selectedBoot.entry || direct.data.entry!=selected.entry ||
       direct.frontBytes!=a.firstUsable*512 || direct.backStart!=(a.lastUsable+1)*512 ||
       direct.backBytes!=sectors*512-direct.backStart)return 1;
    if(!storePath)return 0;
    std::ifstream store(storePath,std::ios::binary);uint8_t sb[1024]{};
    // The same path explicitly means the ext2 fixture is embedded in the
    // GPT-selected partition; a separate path is a standalone store image.
    const uint64_t base=std::string(path)==storePath ? want.first*512 : 0;
    store.seekg(base+1024);store.read(reinterpret_cast<char*>(sb),sizeof(sb));if(!store)return 1;
    std::memcpy(want.filesystemUuid,sb+104,16);
    LargeExt2Geometry g{};if(!LargeExt2GeometryMake(LargeExt2Read32(sb+4),g))return 1;
    std::vector<uint8_t> table(g.descriptorBytes);store.seekg(base+4096);
    store.read(reinterpret_cast<char*>(table.data()),table.size());if(!store)return 1;
    auto inspect=[&](const uint8_t* sbuf,const uint8_t* gbuf,uint32_t n,AhciDataInspection& out){
        return AhciInspectBootAndData(boot,want,sectors,mbr,primary,backup,entries.data(),copy.data(),bytes,
                                      sbuf,1024,gbuf,n,out);
    };
    AhciDataInspection out{};
    assert(inspect(sb,table.data(),table.size(),out)==AhciDataInspectionState::Clean && out.data.entry==2);
    want.filesystemUuid[0]^=1;
    assert(inspect(sb,table.data(),table.size(),out)==AhciDataInspectionState::Invalid && !out.data.entry);
    want.filesystemUuid[0]^=1;sb[58]=0;
    assert(inspect(sb,nullptr,0,out)==AhciDataInspectionState::NeedsCheck && out.data.entry==2);
    sb[58]=1;
    assert(inspect(sb,table.data(),table.size()-1,out)==AhciDataInspectionState::Invalid && !out.data.entry);
    table[0]=0;
    assert(inspect(sb,table.data(),table.size(),out)==AhciDataInspectionState::Invalid && !out.data.entry);
    if(base) {
        struct Reader { std::ifstream disk; int reads=0, failAt=0; bool shortRead=false, dirty=false; uint64_t bytes=0; } reader;
        reader.disk.open(path,std::ios::binary);
        reader.bytes=sectors*512;
        AhciDataReadIo io{&reader,[](void* context,uint64_t offset,void* buffer,uint32_t n)->int {
            auto& r=*static_cast<Reader*>(context);++r.reads;
            assert(offset<=r.bytes && n<=r.bytes-offset);
            if(r.reads==r.failAt)return r.shortRead ? int(n-1) : -5;
            r.disk.clear();r.disk.seekg(offset);r.disk.read(static_cast<char*>(buffer),n);
            if(r.disk && r.dirty && r.reads==6)static_cast<uint8_t*>(buffer)[58]=0;
            return r.disk ? int(n) : -5;
        }};
        std::vector<uint8_t> scratchEntries(1024*1024),scratchBackup(1024*1024),scratchGdt(4096*32);
        auto readInspect=[&](){return AhciReadAndInspectBootData(io,boot,want,sectors,scratchEntries.data(),
            scratchBackup.data(),scratchEntries.size(),scratchGdt.data(),scratchGdt.size(),out);};
        assert(readInspect()==AhciDataInspectionState::Clean && out.data.entry==2 && reader.reads==7);
        for(int failure=1;failure<=7;++failure)for(bool shortRead:{false,true}) {
            reader.reads=0;reader.failAt=failure;reader.shortRead=shortRead;out.data.entry=99;
            assert(readInspect()==AhciDataInspectionState::Invalid && !out.data.entry);
        }
        reader.reads=reader.failAt=0;
        assert(AhciReadAndInspectBootData(io,boot,want,sectors,scratchEntries.data(),scratchBackup.data(),1,
               scratchGdt.data(),scratchGdt.size(),out)==AhciDataInspectionState::Invalid && !out.data.entry);
        reader.reads=0;
        assert(AhciReadAndInspectBootData(io,boot,want,sectors,scratchEntries.data(),scratchBackup.data(),
               scratchEntries.size(),scratchGdt.data(),1,out)==AhciDataInspectionState::Invalid && !out.data.entry);
        reader.reads=0;reader.dirty=true;
        assert(AhciReadAndInspectBootData(io,boot,want,sectors,scratchEntries.data(),scratchBackup.data(),
               scratchEntries.size(),scratchGdt.data(),1,out)==AhciDataInspectionState::NeedsCheck &&
               out.data.entry==2 && reader.reads==6);
    }
    return 0;
}
int main(int argc,char** argv){
    if(argc==2 || argc==3 || argc==4)
        return fixture(argv[1],argc>=3?argv[2]:nullptr,argc==4?std::stoull(argv[3]):32768);
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
    const uint8_t linuxFs[16]={0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
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
    entries[128]^=1;
    // The old implementation used the canonical GUID's first word in the
    // wrong byte order; a checksummed table containing it is not Linux type.
    entries[128]=0x0f;entries[129]=0xc6;entries[130]=0x3d;entries[131]=0xaf;
    refresh();assert(!AhciDataSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));
    std::memcpy(entries+128,linuxFs,16);refresh();
    std::memcpy(entries+256,linuxFs,16);entries[272]=2;put64(entries+288,5000);put64(entries+296,6000);
    refresh();assert(!AhciDataSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));
    entries[272]=4;put64(entries+288,3000);refresh();
    assert(!AhciDataSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));
    const uint8_t espType[16]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0,0xa0,0xc9,0x3e,0xc9,0x3b};
    std::memcpy(entries+256,espType,16);put64(entries+288,5000);put64(entries+296,6000);refresh();
    AhciBootSelector boot{};boot.diskGuid[0]=1;boot.partitionGuid[0]=4;boot.first=5000;boot.count=1001;
    AhciBootSelection bootResult{},dataResult{};
    assert(AhciSelectBootAndData(boot,want,100000,mbr,primary,backup,entries,copy,sizeof(entries),bootResult,dataResult));
    assert(bootResult.entry==3 && dataResult.entry==2);
    AhciDirectLayout layout{};
    assert(AhciDirectLayoutSelect(boot,want,100000,mbr,primary,backup,entries,copy,sizeof(entries),layout));
    assert(layout.esp.entry==3 && layout.data.entry==2 && layout.frontBytes==34*512ULL &&
           layout.backStart==99967*512ULL && layout.backBytes==33*512ULL);
    BlockWriteLease leases[3]{};
    assert(AhciDirectLayoutLeases(layout,1,2,leases));
    BlockWriteLeases protections;assert(protections.Install(leases,3,true));
    assert(!protections.AllowOrdinary(1,0,512));
    assert(!protections.AllowOrdinary(1,layout.data.first*512,512));
    assert(protections.AllowOwner(1,2,layout.data.first*512,512));
    assert(protections.AllowOrdinary(1,layout.esp.first*512,512));
    assert(!protections.AllowOrdinary(1,layout.backStart,512));
    assert(!AhciDirectLayoutLeases(layout,1,1,leases));
    copy[0]^=1;
    assert(!AhciDirectLayoutSelect(boot,want,100000,mbr,primary,backup,entries,copy,sizeof(entries),layout));
    assert(!layout.esp.entry && !layout.data.entry);copy[0]^=1;
    uint8_t sb[1024]{},gdt[32]{};put32(sb,1024);put32(sb+4,256);put32(sb+12,200);put32(sb+16,1000);
    put32(sb+24,2);put32(sb+28,2);put32(sb+32,32768);put32(sb+36,32768);put32(sb+40,1024);
    put16(sb+56,0xef53);put16(sb+58,1);put32(sb+76,1);put32(sb+84,11);put16(sb+88,128);
    put32(sb+96,2);put32(sb+100,3);sb[104]=3;put32(gdt,2);put32(gdt+4,3);put32(gdt+8,4);
    AhciDataInspection inspected{};
    auto inspect=[&](){return AhciInspectBootAndData(boot,want,100000,mbr,primary,backup,entries,copy,sizeof(entries),sb,sizeof(sb),gdt,sizeof(gdt),inspected);};
    assert(inspect()==AhciDataInspectionState::Clean && inspected.esp.entry==3 && inspected.data.entry==2);
    sb[104]^=1;assert(inspect()==AhciDataInspectionState::Invalid && !inspected.data.entry);sb[104]^=1;
    put16(sb+58,0);assert(inspect()==AhciDataInspectionState::NeedsCheck);put16(sb+58,1);
    put32(gdt,1);assert(inspect()==AhciDataInspectionState::Invalid && !inspected.data.entry);put32(gdt,2);
    ++want.count;bootResult.entry=5;dataResult.entry=6;
    assert(!AhciSelectBootAndData(boot,want,100000,mbr,primary,backup,entries,copy,sizeof(entries),bootResult,dataResult));
    assert(!bootResult.entry && !dataResult.entry);--want.count;
    boot.diskGuid[0]=9;
    assert(!AhciSelectBootAndData(boot,want,100000,mbr,primary,backup,entries,copy,sizeof(entries),bootResult,dataResult));
    assert(!bootResult.entry && !dataResult.entry);boot.diskGuid[0]=1;
    ++boot.count;assert(!AhciSelectBootAndData(boot,want,100000,mbr,primary,backup,entries,copy,sizeof(entries),bootResult,dataResult));--boot.count;
    assert(!AhciSelectBootAndData(boot,want,100000,mbr,primary,backup,entries,copy,sizeof(entries),bootResult,bootResult));
    std::memset(entries+256,0,128);refresh();mbr[466]=0x83;
    assert(!AhciDataSelect(want,100000,mbr,primary,backup,entries,copy,sizeof(entries),selected));
}
