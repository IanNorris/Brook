#include "../../kernel/src/strict_gpt_inventory.h"
#include "../../bootloader/src/recovery_disk_gate.h"
#include "../../bootloader/src/recovery_device_path.h"
#include "../../bootloader/src/recovery_disk_read.h"
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
    {
        using namespace brook::bootloader;
        uint8_t path[52]{};path[0]=1;path[1]=1;path[2]=6; // PCI device node
        path[6]=4;path[7]=1;path[8]=42;put32(path+10,1);put64(path+14,2048);
        put64(path+22,2048);path[30]=42;path[46]=2;path[47]=2;
        path[48]=0x7f;path[49]=0xff;path[50]=4;
        uint8_t parent[10]={1,1,6,0,0,0,0x7f,0xff,4,0};RecoveryEspPath parsed{};
        CHECK(RecoveryParseEspPath(path,sizeof(path),parsed) && parsed.partitionNumber==1 &&
              parsed.first==2048 && parsed.sectors==2048 && parsed.guid[0]==42);
        CHECK(RecoveryParentPathMatches(path,sizeof(path),parsed,parent,sizeof(parent)));
        RecoveryEspPath forged=parsed;forged.parentPrefixBytes=~uint32_t{0}-3;
        CHECK(!RecoveryParentPathMatches(path,sizeof(path),forged,parent,0));
        forged.parentPrefixBytes=0;CHECK(!RecoveryParentPathMatches(path,sizeof(path),forged,parent+6,4));
        CHECK(!RecoveryParentPathMatches(path,sizeof(path),parsed,parent,sizeof(parent)-1));
        parent[1]^=1;CHECK(!RecoveryParentPathMatches(path,sizeof(path),parsed,parent,sizeof(parent)));parent[1]^=1;
        parent[7]=1;CHECK(!RecoveryParentPathMatches(path,sizeof(path),parsed,parent,sizeof(parent)));parent[7]=0xff;
        path[47]=1;CHECK(!RecoveryParseEspPath(path,sizeof(path),parsed));path[47]=2;
        path[2]=0;CHECK(!RecoveryParseEspPath(path,sizeof(path),parsed));path[2]=6;
        path[49]=1;CHECK(!RecoveryParseEspPath(path,sizeof(path),parsed));path[49]=0xff;
        path[0]=4;CHECK(!RecoveryParseEspPath(path,sizeof(path),parsed));path[0]=1;
        put32(path+10,0);CHECK(!RecoveryParseEspPath(path,sizeof(path),parsed));put32(path+10,1);
        put64(path+22,0);CHECK(!RecoveryParseEspPath(path,sizeof(path),parsed));put64(path+22,2048);
        path[30]=0;CHECK(!RecoveryParseEspPath(path,sizeof(path),parsed));path[30]=42;
        CHECK(!RecoveryParseEspPath(path,sizeof(path)-1,parsed));
        put64(path+14,~uint64_t{0});CHECK(!RecoveryParseEspPath(path,sizeof(path),parsed));
    }
    {
        using namespace brook::bootloader;
        const uint8_t espType[16]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0,0xa0,0xc9,0x3e,0xc9,0x3b};
        uint8_t ae[16384]{},be[16384]{},ph[512]{},bh[512]{},protective[512]{};
        partition(ae,1,2048,4095);partition(ae+128,2,4096,8191);
        std::memcpy(ae,espType,16);
        // GPT encoding of canonical Linux type 0fc63daf-8483-4772-8e79-3d69d8477de4.
        put32(ae+128,0x0fc63daf);ae[132]=0x83;ae[133]=0x84;ae[134]=0x72;ae[135]=0x47;
        const uint8_t tail[8]={0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};std::memcpy(ae+136,tail,8);
        std::memcpy(be,ae,sizeof(ae));const uint32_t sum=StrictCrc32(ae,sizeof(ae));
        header(ph,false,sum);header(bh,true,sum);
        protective[510]=0x55;protective[511]=0xaa;protective[450]=0xee;
        put32(protective+454,1);put32(protective+458,99999);
        RecoveryDiskIdentity id{};id.diskGuid[0]=1;id.espGuid[0]=1;id.dataGuid[0]=2;id.fsUuid[0]=3;
        id.espFirst=2048;id.espLast=4095;id.espPartitionNumber=1;id.dataFirst=4096;id.dataSectors=4096;
        RecoveryDataPartition data{};
        auto locate=[&]{return RecoveryLocateData(protective,ph,bh,ae,be,sizeof(ae),100000,id,data);};
        CHECK(locate() && data.first==4096 && data.sectors==4096);
        uint8_t sb[1024]{};put32(sb,64);put32(sb+4,512);put32(sb+12,512);put32(sb+16,64);
        put32(sb+24,2);put32(sb+28,2);put32(sb+32,32768);put32(sb+36,32768);
        put32(sb+40,64);sb[56]=0x53;sb[57]=0xef;sb[58]=1;put32(sb+76,1);
        put32(sb+84,11);sb[88]=128;put32(sb+96,2);put32(sb+100,3);sb[104]=3;
        CHECK(RecoveryDataHeaderIsClean(sb,sizeof(sb),id,data));
        struct FakeReader {
            uint8_t *mbr,*ph,*bh,*ae,*be,*sb;unsigned calls=0,failAt=0,stableCalls=0;bool stable=true,changeAfterRead=false;
            bool Stable(){++stableCalls;return stable && !(changeAfterRead && stableCalls>1);}
            bool Read(uint64_t lba,uint32_t count,void* out) {
                if(++calls==failAt)return false;
                const uint8_t* source=nullptr;
                if(lba==0 && count==1)source=mbr;
                else if(lba==1 && count==1)source=ph;
                else if(lba==99999 && count==1)source=bh;
                else if(lba==2 && count==32)source=ae;
                else if(lba==99967 && count==32)source=be;
                else if(lba==4098 && count==2)source=sb;
                if(!source)return false;
                std::memcpy(out,source,count*512);return true;
            }
        } reader{protective,ph,bh,ae,be,sb};
        uint8_t scratchA[16384]{},scratchB[16384]{};
        auto readState=[&]{return RecoveryReadDiskState(reader,100000,id,scratchA,scratchB,sizeof(scratchA));};
        CHECK(readState()==RecoveryDiskState::Clean);
        sb[58]=0;CHECK(readState()==RecoveryDiskState::NeedsCheck);sb[58]=1;
        for(unsigned fail=1;fail<=6;++fail) {
            reader.calls=0;reader.failAt=fail;CHECK(readState()==RecoveryDiskState::Invalid);
        }
        reader.failAt=0;reader.stable=false;CHECK(readState()==RecoveryDiskState::Invalid);reader.stable=true;
        reader.stableCalls=0;reader.changeAfterRead=true;
        CHECK(readState()==RecoveryDiskState::Invalid && reader.stableCalls==2);reader.changeAfterRead=false;
        CHECK(RecoveryReadDiskState(reader,100000,id,scratchA,scratchA,sizeof(scratchA))==RecoveryDiskState::Invalid);
        CHECK(RecoveryReadDiskState(reader,100000,id,reinterpret_cast<uint8_t*>(&id),scratchB,sizeof(scratchB))==RecoveryDiskState::Invalid);
        CHECK(RecoveryReadDiskState(reader,100000,id,scratchA,scratchB,512)==RecoveryDiskState::Invalid);
        sb[58]=0;CHECK(!RecoveryDataHeaderIsClean(sb,sizeof(sb),id,data));sb[58]=1;
        sb[104]^=1;CHECK(!RecoveryDataHeaderIsClean(sb,sizeof(sb),id,data));sb[104]^=1;
        id.espPartitionNumber=2;CHECK(!locate());id.espPartitionNumber=1;
        id.espFirst++;CHECK(!locate());id.espFirst--;
        id.dataFirst++;CHECK(!locate());id.dataFirst--;
        id.dataSectors--;CHECK(!locate() && data.first==0 && data.sectors==0);id.dataSectors++;
        id.dataSectors=3;put64(ae+128+40,4098);std::memcpy(be,ae,sizeof(ae));
        header(ph,false,StrictCrc32(ae,sizeof(ae)));header(bh,true,StrictCrc32(ae,sizeof(ae)));
        CHECK(!locate());
        id.dataSectors=4096;put64(ae+128+40,8191);std::memcpy(be,ae,sizeof(ae));
        header(ph,false,StrictCrc32(ae,sizeof(ae)));header(bh,true,StrictCrc32(ae,sizeof(ae)));
        id.diskGuid[0]^=1;CHECK(!locate());id.diskGuid[0]^=1;
        id.dataGuid[0]^=4;CHECK(!locate());id.dataGuid[0]^=4;
        be[200]^=1;CHECK(!locate());be[200]^=1;
        ae[128]^=1;std::memcpy(be,ae,sizeof(ae));
        header(ph,false,StrictCrc32(ae,sizeof(ae)));header(bh,true,StrictCrc32(ae,sizeof(ae)));
        CHECK(!locate());
        protective[450]=0;CHECK(!locate());
    }
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
