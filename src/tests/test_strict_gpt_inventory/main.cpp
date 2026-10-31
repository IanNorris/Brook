#include "../../kernel/src/strict_gpt_inventory.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
using namespace brook;

static int failures;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); ++failures; } } while (0)
static void put32(uint8_t* p, uint32_t v) { for (int i=0;i<4;++i) p[i]=uint8_t(v>>(8*i)); }
static void put64(uint8_t* p, uint64_t v) { put32(p,uint32_t(v)); put32(p+4,uint32_t(v>>32)); }
static void checksum(uint8_t* h) { put32(h+16,0);put32(h+16,StrictCrc32(h,92,true)); }
static void header(uint8_t* h, bool backup, uint32_t crc) {
    std::memset(h,0,512);std::memcpy(h,"EFI PART",8);put32(h+8,0x10000);put32(h+12,92);
    put64(h+24,backup?99999:1);put64(h+32,backup?1:99999);
    put64(h+40,34);put64(h+48,99966);h[56]=1;put64(h+72,backup?99967:2);
    put32(h+80,128);put32(h+84,128);put32(h+88,crc);checksum(h);
}
static void partition(uint8_t* e,uint8_t id,uint64_t first,uint64_t last) {
    e[0]=0xaf;e[16]=id;put64(e+32,first);put64(e+40,last);
}
static int CheckImage(const char* path) {
    std::ifstream f(path,std::ios::binary);if(!f)return 1;
    f.seekg(0,std::ios::end);const auto end=f.tellg();if(end<0 || uint64_t(end)%512)return 1;
    const uint64_t sectors=uint64_t(end)/512;if(sectors<68)return 1;
    uint8_t mbr[512],first[512],last[512];f.seekg(0);f.read(reinterpret_cast<char*>(mbr),512);
    if(StrictGptProtectiveMbr(mbr,sectors)!=StrictMbrResult::Protective)return 1;
    f.seekg(512);f.read(reinterpret_cast<char*>(first),512);
    f.seekg((sectors-1)*512);f.read(reinterpret_cast<char*>(last),512);if(!f)return 1;
    StrictGptHeader a{},b{};
    if(StrictGptCheckHeader(first,512,sectors,false,a)!=StrictGptResult::Ok ||
       StrictGptCheckHeader(last,512,sectors,true,b)!=StrictGptResult::Ok)return 1;
    const uint32_t bytes=a.count*a.entrySize;std::vector<uint8_t> ae(bytes),be(bytes);
    if(b.count*b.entrySize!=bytes)return 1;
    f.seekg(a.entriesLba*512);f.read(reinterpret_cast<char*>(ae.data()),bytes);
    f.seekg(b.entriesLba*512);f.read(reinterpret_cast<char*>(be.data()),bytes);
    if(!f || StrictGptCheckCopies(a,ae.data(),b,be.data(),bytes)!=StrictGptResult::Ok)return 1;
    std::puts("external GPT fixture: PASS");return 0;
}
int main(int argc,char** argv) {
    if(argc==2)return CheckImage(argv[1]);
    if(argc!=1)return 1;
    CHECK(StrictCrc32(reinterpret_cast<const uint8_t*>("123456789"),9)==0xcbf43926u);
    uint8_t mbr[512]={};mbr[510]=0x55;mbr[511]=0xaa;mbr[446+4]=0xee;put32(mbr+446+8,1);put32(mbr+446+12,99999);
    CHECK(StrictGptProtectiveMbr(mbr,100000)==StrictMbrResult::Protective);
    mbr[462+4]=0x83;CHECK(StrictGptProtectiveMbr(mbr,100000)==StrictMbrResult::Hybrid);mbr[462+4]=0;
    put32(mbr+446+8,2);CHECK(StrictGptProtectiveMbr(mbr,100000)==StrictMbrResult::Invalid);
    uint8_t entries[16384]={},backupEntries[16384]={},primary[512],backup[512];
    partition(entries,1,2048,4095);partition(entries+128,2,4096,8191);
    std::memcpy(backupEntries,entries,sizeof(entries));uint32_t crc=StrictCrc32(entries,sizeof(entries));
    header(primary,false,crc);header(backup,true,crc);StrictGptHeader a{},b{};
    CHECK(StrictGptCheckHeader(primary,512,100000,false,a)==StrictGptResult::Ok);
    CHECK(StrictGptCheckHeader(backup,512,100000,true,b)==StrictGptResult::Ok);
    CHECK(StrictGptCheckCopies(a,entries,b,backupEntries,sizeof(entries))==StrictGptResult::Ok);
    CHECK(StrictGptCheckHeader(primary,4096,100000,false,a)==StrictGptResult::UnsupportedGeometry);
    primary[56]^=1;CHECK(StrictGptCheckHeader(primary,512,100000,false,a)==StrictGptResult::HeaderChecksum);primary[56]^=1;
    put64(primary+72,33);checksum(primary);CHECK(StrictGptCheckHeader(primary,512,100000,false,a)==StrictGptResult::InvalidLayout);
    header(primary,false,crc);put32(primary+84,384);checksum(primary);
    CHECK(StrictGptCheckHeader(primary,512,100000,false,a)==StrictGptResult::InvalidLayout);
    header(primary,false,crc);put64(primary+40,3);checksum(primary);
    CHECK(StrictGptCheckHeader(primary,512,100000,false,a)==StrictGptResult::InvalidLayout);
    header(primary,false,crc);put64(primary+32,99998);checksum(primary);
    CHECK(StrictGptCheckHeader(primary,512,100000,false,a)==StrictGptResult::InvalidLayout);
    header(primary,false,crc);put32(primary+80,0);checksum(primary);
    CHECK(StrictGptCheckHeader(primary,512,100000,false,a)==StrictGptResult::InvalidLayout);
    header(primary,false,crc);put64(primary+72,~uint64_t{0});checksum(primary);
    CHECK(StrictGptCheckHeader(primary,512,100000,false,a)==StrictGptResult::InvalidLayout);
    header(primary,false,crc);primary[511]=1;
    CHECK(StrictGptCheckHeader(primary,512,100000,false,a)==StrictGptResult::InvalidHeader);
    header(backup,true,crc);put64(backup+72,99960);checksum(backup);
    CHECK(StrictGptCheckHeader(backup,512,100000,true,b)==StrictGptResult::InvalidLayout);
    header(backup,true,crc);CHECK(StrictGptCheckHeader(backup,512,100000,true,b)==StrictGptResult::Ok);
    header(primary,false,crc);CHECK(StrictGptCheckHeader(primary,512,100000,false,a)==StrictGptResult::Ok);
    backupEntries[200]^=1;CHECK(StrictGptCheckCopies(a,entries,b,backupEntries,sizeof(entries))==StrictGptResult::EntryChecksum);backupEntries[200]^=1;
    b.diskGuid[0]^=1;CHECK(StrictGptCheckCopies(a,entries,b,backupEntries,sizeof(entries))==StrictGptResult::CopiesDiffer);b.diskGuid[0]^=1;
    put64(entries+128+32,4095);std::memcpy(backupEntries,entries,sizeof(entries));a.entriesCrc=b.entriesCrc=StrictCrc32(entries,sizeof(entries));
    CHECK(StrictGptCheckCopies(a,entries,b,backupEntries,sizeof(entries))==StrictGptResult::OverlappingPartitions);
    put64(entries+128+32,4096);entries[128+16]=1;std::memcpy(backupEntries,entries,sizeof(entries));a.entriesCrc=b.entriesCrc=StrictCrc32(entries,sizeof(entries));
    CHECK(StrictGptCheckCopies(a,entries,b,backupEntries,sizeof(entries))==StrictGptResult::InvalidPartition);
    std::printf("strict GPT inventory: %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
