#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <sys/mman.h>
#include "acpi.h"
#include "acpi_embedded_controller.h"
#include "boot_protocol.h"
namespace brook {
extern "C" void SerialPrintf(const char*,...){}
extern "C" void SerialPuts(const char*){}
}
using namespace brook;
struct Firmware {
    uint8_t* memory=static_cast<uint8_t*>(mmap(nullptr,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0));
    MemoryDescriptor map{};
    uint8_t *rsdp,*xsdt,*rsdt,*fadt,*madt;
    Firmware(){assert(memory!=MAP_FAILED);rsdp=memory+256;xsdt=memory+4096;rsdt=xsdt+256;fadt=memory+8192;madt=memory+12288;reset();}
    ~Firmware(){assert(!munmap(memory,65536));}
    static uint64_t address(void* p){return reinterpret_cast<uint64_t>(p);}
    static void put32(void* p,uint32_t v){std::memcpy(p,&v,4);}
    static void put64(void* p,uint64_t v){std::memcpy(p,&v,8);}
    static void checksum(uint8_t* p,unsigned bytes,unsigned offset){p[offset]=0;unsigned sum=0;for(unsigned i=0;i<bytes;++i)sum+=p[i];p[offset]=uint8_t(-sum);}
    static void header(uint8_t* p,const char* sig,unsigned bytes){std::memcpy(p,sig,4);put32(p+4,bytes);p[8]=6;}
    void fadtCrc(unsigned bytes=276){checksum(fadt,bytes,9);}
    void madtCrc(unsigned bytes=64){checksum(madt,bytes,9);}
    void rsdpCrc(){checksum(rsdp,20,8);checksum(rsdp,36,32);}
    void reset(){
        std::memset(memory,0,65536);map={address(memory),16,MemoryType::AcpiReclaimable,0};
        std::memcpy(rsdp,"RSD PTR ",8);rsdp[15]=2;put32(rsdp+16,address(rsdt));put32(rsdp+20,36);put64(rsdp+24,address(xsdt));rsdpCrc();
        header(xsdt,"XSDT",52);put64(xsdt+36,address(fadt));put64(xsdt+44,address(madt));checksum(xsdt,52,9);
        header(rsdt,"RSDT",44);put32(rsdt+36,address(fadt));put32(rsdt+40,address(madt));checksum(rsdt,44,9);
        header(fadt,"FACP",276);put32(fadt+112,1u<<10);fadt[116]=1;fadt[117]=8;fadt[119]=1;put64(fadt+120,0xcf9);fadt[128]=6;fadtCrc();
        header(madt,"APIC",64);put32(madt+36,0xfee00000);put32(madt+40,1);
        madt[44]=0;madt[45]=8;madt[46]=3;madt[47]=2;put32(madt+48,1);
        madt[52]=1;madt[53]=12;madt[54]=1;put32(madt+56,0xfec00000);madtCrc();
    }
    bool init(){return AcpiInit(address(rsdp),&map,1);}
    void noReset(){assert(init());assert(!AcpiGetResetInfo().available);}
};
static void TestLegacyPower(Firmware& f) {
    f.reset();
    auto* facs=f.memory+20480;
    Firmware::header(facs,"FACS",64);
    Firmware::put32(f.fadt+36,Firmware::address(facs));
    f.fadt[46]=9;
    Firmware::put32(f.fadt+48,0xb2);
    f.fadt[52]=0xd1;
    Firmware::put32(f.fadt+64,0x1804);
    f.fadt[89]=2;
    f.fadtCrc();
    assert(f.init() && !AcpiGetLegacyPowerInfo().facs); // needs ACPI NVS
    f.map.type=MemoryType::AcpiNvs;
    assert(f.init() && AcpiGetLegacyPowerInfo().facs==Firmware::address(facs));
    assert(AcpiGetLegacyPowerInfo().pm1aControl==0x1804);
    assert(AcpiGetLegacyPowerInfo().smiCommand==0xb2 && AcpiGetLegacyPowerInfo().acpiEnable==0xd1);
    assert(AcpiGetSciRoute().available && AcpiGetSciRoute().gsi==9 && AcpiGetSciRoute().activeLow);
    const uint8_t dellIso[]={2,10,0,9,9,0,0,0,13,0}; // Dell routes SCI level/high
    std::memcpy(f.madt+64,dellIso,sizeof(dellIso));
    Firmware::put32(f.madt+4,74);f.madtCrc(74);
    assert(f.init() && AcpiGetSciRoute().available && !AcpiGetSciRoute().activeLow);
    f.madt[72]=5;f.madtCrc(74); // edge/high is not supported for SCI
    assert(f.init() && !AcpiGetSciRoute().available);
    f.madt[72]=13;
    std::memcpy(f.madt+74,dellIso,sizeof(dellIso));
    Firmware::put32(f.madt+4,84);f.madtCrc(84);
    assert(f.init() && !AcpiGetSciRoute().available);
    f.fadt[172]=0;f.fadt[173]=16;f.fadt[175]=2; // unsupported SystemMemory control GAS
    Firmware::put64(f.fadt+176,0x1804);f.fadtCrc();
    assert(f.init() && !AcpiGetLegacyPowerInfo().facs);
    f.fadt[172]=1;f.fadtCrc();
    assert(f.init() && AcpiGetLegacyPowerInfo().facs);
    Firmware::put64(f.fadt+132,Firmware::address(facs)+64);f.fadtCrc();
    assert(f.init() && !AcpiGetLegacyPowerInfo().facs);
    Firmware::put64(f.fadt+132,0);f.fadtCrc();
    Firmware::put32(facs+4,65536);
    assert(f.init() && !AcpiGetLegacyPowerInfo().facs);
    f.reset();
    assert(f.init());
}
static void TestEmbeddedController() {
    struct Io {
        uint8_t trace[3]{};
        uint8_t traceCount=0,reads=0,forcedStatus=0;
        unsigned polls=0;
        uint64_t now=0;
        bool stuck=false,clockStuck=false;
        uint64_t nowUs() const {return now;}
        void delayPoll(){if(!clockStuck)now+=10;}
        uint8_t status(){
            ++polls;
            if(forcedStatus)return forcedStatus;
            if(stuck && traceCount)return 2;
            if(traceCount==2 && trace[0]==0x80)return 1;
            return polls%2?2:0;
        }
        void command(uint8_t byte){assert(!traceCount);trace[traceCount++]=byte;}
        void writeData(uint8_t byte){assert(traceCount && traceCount<3);trace[traceCount++]=byte;}
        uint8_t readData(){++reads;return 0x45;}
    };
    AcpiEmbeddedController controller;
    uint8_t value=0;
    Io input;
    assert(controller.Read(input,6,value)==AcpiEcResult::Ok && value==0x45);
    assert(input.traceCount==2 && input.trace[0]==0x80 && input.trace[1]==6 && input.reads==1);
    Io output;
    assert(controller.Write(output,1,5)==AcpiEcResult::Ok);
    assert(output.traceCount==3 && output.trace[0]==0x81 && output.trace[1]==1 && output.trace[2]==5 && !output.reads);
    Io stale;stale.forcedStatus=1;
    assert(controller.Read(stale,6,value)==AcpiEcResult::UnexpectedOutput && !stale.traceCount && !stale.reads);
    Io missing;missing.forcedStatus=0xff;
    assert(controller.Read(missing,6,value)==AcpiEcResult::Unavailable && !missing.traceCount);
    Io busy;busy.forcedStatus=2;busy.clockStuck=true;
    assert(controller.Read(busy,6,value)==AcpiEcResult::Busy && busy.polls==5000 && !busy.traceCount);
    assert(!controller.Quarantined());
    Io lost;lost.stuck=true;
    assert(controller.Read(lost,6,value)==AcpiEcResult::TimedOut && controller.Quarantined());
    assert(lost.traceCount==1 && !lost.reads && value==0x45);
    Io after;
    assert(controller.Read(after,6,value)==AcpiEcResult::Unavailable && !after.polls);
}
int main(){
    Firmware f;assert(f.init());auto reset=AcpiGetResetInfo();assert(reset.available && reset.port==0xcf9 && reset.value==6);
    TestLegacyPower(f);
    TestEmbeddedController();
    assert(!AcpiGetPmTimerInfo().available);
    Firmware::put32(f.fadt+76,0x408);f.fadt[91]=4;f.fadtCrc();assert(f.init());
    assert(AcpiGetPmTimerInfo().available && AcpiGetPmTimerInfo().port==0x408 && !AcpiGetPmTimerInfo().wide);
    Firmware::put32(f.fadt+112,(1u<<10)|(1u<<8));f.fadt[208]=1;f.fadt[209]=32;f.fadt[211]=3;
    Firmware::put64(f.fadt+212,0x508);f.fadtCrc();assert(f.init());
    assert(AcpiGetPmTimerInfo().available && AcpiGetPmTimerInfo().port==0x508 && AcpiGetPmTimerInfo().wide);
    // Same System-I/O GAS layout as the previously captured Dell FADT.
    Firmware::put32(f.fadt+76,0x1808);Firmware::put32(f.fadt+112,0x386a5);
    Firmware::put64(f.fadt+212,0x1808);f.fadtCrc();assert(f.init());
    assert(AcpiGetPmTimerInfo().available && AcpiGetPmTimerInfo().port==0x1808 && !AcpiGetPmTimerInfo().wide);
    Firmware::put32(f.fadt+76,0x408);
    f.fadt[208]=0;f.fadtCrc();assert(f.init() && AcpiGetPmTimerInfo().port==0x408);
    f.fadt[91]=3;f.fadtCrc();assert(f.init() && !AcpiGetPmTimerInfo().available);
    f.reset();assert(f.init());
    // Enumeration retains duplicate root entries and exposes DSDT only via a
    // checksum-validated FADT reference. Firmware spans remain bounded.
    {
        auto* dsdt=f.memory+16384;Firmware::header(dsdt,"DSDT",64);Firmware::checksum(dsdt,64,9);
        Firmware::put32(f.fadt+40,Firmware::address(dsdt));Firmware::put64(f.fadt+140,Firmware::address(dsdt));f.fadtCrc();
        AcpiTableReader reader;
        auto read=[](void* ctx,uint64_t at,void* out,uint32_t n)->bool {
            auto* firmware=static_cast<Firmware*>(ctx);const auto start=Firmware::address(firmware->memory);
            if(at<start || at-start>65536 || n>65536-(at-start))return false;
            std::memcpy(out,reinterpret_cast<void*>(at),n);return true;
        };
        assert(reader.Init(read,&f,Firmware::address(f.rsdp)) && reader.RootEntryCount()==2);
        AcpiSdtHeader table{};assert(reader.RootEntry(0,&table)==Firmware::address(f.fadt));
        assert(reader.RootEntry(1)==Firmware::address(f.madt) && !reader.RootEntry(2));
        assert(reader.Dsdt(&table)==Firmware::address(dsdt) && table.length==64);
        uint8_t copy[8]{};assert(reader.CopyTable(reader.Dsdt(),0,copy,sizeof(copy)) && !std::memcmp(copy,"DSDT",4));
        assert(f.init() && AcpiInventoryCount()==3);
        assert(AcpiInventoryRead(0,0,copy,sizeof(copy),table) && !std::memcmp(copy,"DSDT",4));
        assert(!AcpiInventoryRead(1,0,copy,0,table)); // FADT is not an AML table
        std::memcpy(f.fadt,"MSDM",4);f.fadtCrc();assert(!AcpiInventoryRead(1,0,copy,4,table));
        std::memcpy(f.fadt,"SSDT",4);f.fadtCrc();assert(AcpiInventoryRead(1,0,copy,4,table) && !std::memcmp(copy,"SSDT",4));
        std::memcpy(f.fadt,"DMAR",4);f.fadtCrc();assert(AcpiInventoryRead(1,0,copy,4,table) && !std::memcmp(copy,"DMAR",4));
        uint8_t dmar[300]{};uint32_t dmarBytes=0;
        assert(AcpiUniqueDmarCopy(dmar,sizeof(dmar),dmarBytes) && dmarBytes==276 && !std::memcmp(dmar,"DMAR",4));
        assert(!AcpiUniqueDmarCopy(dmar,100,dmarBytes) && dmarBytes==0);
        std::memcpy(f.madt,"DMAR",4);f.madtCrc();assert(!AcpiUniqueDmarCopy(dmar,sizeof(dmar),dmarBytes) && !dmarBytes);
        std::memcpy(f.madt,"APIC",4);f.madtCrc();
        std::memcpy(f.fadt,"WDAT",4);f.fadtCrc();assert(AcpiInventoryRead(1,0,copy,4,table) && !std::memcmp(copy,"WDAT",4));
        f.fadt[9]++;assert(!AcpiInventoryRead(1,0,copy,4,table)); // checksum still required
        assert(!AcpiUniqueDmarCopy(dmar,sizeof(dmar),dmarBytes) && !dmarBytes);
        std::memcpy(f.fadt,"FACP",4);f.fadtCrc();
        assert(!AcpiInventoryRead(3,0,copy,1,table) && !AcpiInventoryRead(0,60,copy,8,table));
        assert(!AcpiInventoryRead(0,0,copy,513,table) && !AcpiInventoryRead(0,0,nullptr,1,table));
        assert(!reader.CopyTable(reader.Dsdt(),60,copy,8) && !reader.CopyTable(reader.Dsdt(),UINT32_MAX,copy,1));
        dsdt[40]^=1;assert(!reader.Dsdt());dsdt[40]^=1;
        Firmware::put64(f.fadt+140,0);f.fadtCrc();assert(reader.Dsdt()==Firmware::address(dsdt));
        Firmware::put64(f.fadt+140,~0ull);f.fadtCrc();assert(!reader.Dsdt());
        Firmware::put64(f.xsdt+44,Firmware::address(f.memory)+65536);Firmware::checksum(f.xsdt,52,9);
        assert(!reader.RootEntry(1));
        f.reset();
    }
    auto madt=AcpiGetMadt();assert(madt.processorCount==1 && madt.apicIds[0]==2 && madt.ioApicPhysical==0xfec00000);
    // Optional FADT failures do not invalidate a sound MADT or issue I/O.
    f.fadt[200]^=1;f.noReset();assert(!AcpiFindTable("FACP"));
    f.reset();Firmware::put32(f.fadt+112,0);f.fadtCrc();f.noReset();
    f.reset();Firmware::put32(f.fadt+4,128);f.fadtCrc(128);f.noReset();
    for(auto byte : {0,2,0x7f}){f.reset();f.fadt[116]=byte;f.fadtCrc();f.noReset();}
    for(auto offset : {117,118,119}){f.reset();f.fadt[offset]=16;f.fadtCrc();f.noReset();}
    for(uint64_t port : {0ULL,65536ULL,~0ULL}){f.reset();Firmware::put64(f.fadt+120,port);f.fadtCrc();f.noReset();}
    f.reset();f.fadt[119]=0;f.fadt[128]=0;f.fadtCrc();assert(f.init() && AcpiGetResetInfo().available && !AcpiGetResetInfo().value);
    f.reset();Firmware::put32(f.fadt+4,AcpiTableReader::MaximumTableBytes+1);f.noReset();
    f.reset();f.rsdp[8]^=1;assert(!f.init());
    f.reset();f.rsdp[32]^=1;assert(!f.init());
    f.reset();Firmware::put32(f.rsdp+20,20);f.rsdpCrc();assert(!f.init());
    f.reset();Firmware::put32(f.rsdp+20,4097);f.rsdpCrc();assert(!f.init());
    f.reset();f.xsdt[9]^=1;assert(!f.init());
    f.reset();Firmware::put32(f.xsdt+4,53);Firmware::checksum(f.xsdt,53,9);assert(!f.init());
    f.reset();Firmware::put32(f.xsdt+4,35);assert(!f.init());
    f.reset();Firmware::put64(f.xsdt+36,~0ULL);Firmware::checksum(f.xsdt,52,9);f.noReset();
    f.reset();Firmware::put64(f.rsdp+24,0);f.rsdpCrc();assert(f.init() && AcpiGetResetInfo().available); // RSDT fallback
    f.reset();f.rsdp[15]=0;Firmware::checksum(f.rsdp,20,8);assert(f.init());
    for(auto type : {MemoryType::Free,MemoryType::Mmio,MemoryType::Kernel,MemoryType::BootData,MemoryType::Unusable}){f.reset();f.map.type=type;assert(!f.init());}
    for(auto type : {MemoryType::Reserved,MemoryType::AcpiNvs}){f.reset();f.map.type=type;assert(f.init());}
    f.reset();assert(f.init());uint8_t nvsRead[8]{};
    assert(!AcpiNvsContains(Firmware::address(f.memory),1) && !AcpiNvsRead(Firmware::address(f.memory),nvsRead,1));
    f.map.type=MemoryType::AcpiNvs;assert(f.init());
    assert(AcpiNvsContains(Firmware::address(f.memory),65536));
    assert(AcpiNvsRead(Firmware::address(f.memory),nvsRead,8) && !std::memcmp(nvsRead,f.memory,8));
    assert(!AcpiNvsRead(Firmware::address(f.memory),nvsRead,9));
    assert(!AcpiNvsRead(Firmware::address(f.memory)+65535,nvsRead,2));
    assert(!AcpiNvsContains(~0ULL,8) && !AcpiNvsContains(Firmware::address(f.memory),0));
    MemoryDescriptor classified[3]={f.map,{0xfe103000,1,MemoryType::Mmio,0},{0xfe104000,1,MemoryType::Reserved,0}};
    assert(AcpiInit(Firmware::address(f.rsdp),classified,3));
    assert(AcpiMmioMapValid());
    BootResourceClassification observed{};
    assert(AcpiClassifyBootRange(0xfe103000,4096,observed) && observed.types==(1u<<uint32_t(MemoryType::Mmio)) && !observed.unreported);
    assert(AcpiClassifyBootRange(0xfe105000,4096,observed) && observed.types==0 && observed.unreported);
    assert(AcpiMmioContains(0xfe103000,4096) && AcpiMmioContains(0xfe103ffc,4));
    assert(!AcpiMmioContains(0xfe103fff,2) && !AcpiMmioContains(0xfe104000,4096));
    assert(!AcpiMmioContains(~0ull,2) && !AcpiMmioContains(0xfe103000,0));
    classified[2].type=MemoryType::Mmio;assert(AcpiInit(Firmware::address(f.rsdp),classified,3));
    assert(AcpiMmioContains(0xfe103000,8192));
    classified[2].physicalStart=0xfe104001;assert(AcpiInit(Firmware::address(f.rsdp),classified,3));
    assert(!AcpiMmioContains(0xfe103000,8192));
    classified[2].pageCount=~0ull;assert(AcpiInit(Firmware::address(f.rsdp),classified,3));
    assert(!AcpiMmioMapValid() && !AcpiMmioContains(0xfe103000,4096));
    MemoryDescriptor many[514]{};many[0]=f.map;
    for(unsigned i=1;i<514;++i)many[i]={0x800000000ull+uint64_t(i)*4096,1,MemoryType::Mmio,0};
    assert(AcpiInit(Firmware::address(f.rsdp),many,514) && !AcpiMmioMapValid());
    assert(f.init() && AcpiMmioMapValid() && !AcpiMmioContains(0xfe103000,4096));
    f.reset();assert(f.init());f.map={};assert(AcpiFindTable("FACP")==Firmware::address(f.fadt)); // boot map was copied
    for(auto length : {0,1,7,65}){f.reset();f.madt[45]=length;f.madtCrc();assert(!f.init());assert(!AcpiGetMadt().processorCount);}
    f.reset();Firmware::put32(f.madt+4,43);f.madtCrc(43);assert(!f.init());
    f.reset();Firmware::put32(f.madt+4,65);f.madtCrc(65);assert(!f.init());
    f.reset();Firmware::put32(f.madt+4,66);f.madt[64]=255;f.madt[65]=2;f.madtCrc(66);assert(f.init());
    f.reset();MemoryDescriptor split[2]={{f.map.physicalStart,1,MemoryType::AcpiReclaimable,0},{f.map.physicalStart+4096,15,MemoryType::Reserved,0}};
    assert(AcpiInit(Firmware::address(f.rsdp),split,2));
    split[1].type=MemoryType::Mmio;assert(!AcpiInit(Firmware::address(f.rsdp),split,2));
    std::puts("ACPI_TABLES_PASS actual-reader RSDP/XSDT/RSDT/checksums/bounds/MADT/reset-GAS/region-lifetime");
}
