#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <sys/mman.h>
#include "acpi.h"
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
int main(){
    Firmware f;assert(f.init());auto reset=AcpiGetResetInfo();assert(reset.available && reset.port==0xcf9 && reset.value==6);
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
