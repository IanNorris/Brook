#include "acpi.h"
#include "acpi_tables.h"
#include "boot_timing.h"
#include "boot_resource_map.h"
#include "boot_protocol/boot_protocol.h"
#include "serial.h"
#ifdef BROOK_DELL_ACPI_MONITOR
#include "string.h"
#include "update/payload.h"
#endif

namespace brook {
struct MadtHeader
{
    AcpiSdtHeader header;
    uint32_t        localApicAddress;  // default LAPIC physical address
    uint32_t        flags;             // bit 0: legacy 8259 PICs installed
} __attribute__((packed));

// MADT entry types
static constexpr uint8_t MADT_TYPE_LAPIC       = 0;
static constexpr uint8_t MADT_TYPE_IOAPIC      = 1;
static constexpr uint8_t MADT_TYPE_LAPIC_OVERRIDE = 5;  // 64-bit LAPIC address

struct MadtEntryHeader
{
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

struct MadtLapicEntry
{
    MadtEntryHeader header;
    uint8_t  processorId;
    uint8_t  apicId;
    uint32_t flags;   // bit 0: enabled
} __attribute__((packed));

struct MadtIoApicEntry
{
    MadtEntryHeader header;
    uint8_t  ioApicId;
    uint8_t  _reserved;
    uint32_t ioApicAddress;
    uint32_t gsiBase;
} __attribute__((packed));

struct MadtLapicOverride
{
    MadtEntryHeader header;
    uint16_t        _reserved;
    uint64_t        lapicAddress;
} __attribute__((packed));


static MadtInfo g_madt{};
static AcpiResetInfo g_reset{};
static AcpiPmTimerInfo g_pmTimer{};
static AcpiLegacyPowerInfo g_legacyPower{};
static AcpiSciRoute g_sciRoute{};
static AcpiTableReader g_tables;
static BootResourceMap g_bootResourceMap;
static bool g_acpiReady=false;
#ifdef BROOK_DELL_ACPI_MONITOR
static uint64_t g_rsdp=0;
#endif
struct FirmwareRegion {uint64_t begin,end;bool nvs;};
static FirmwareRegion g_regions[512]{};
static uint32_t g_regionCount=0;
struct MmioRegion {uint64_t begin,end;};
static MmioRegion g_mmioRegions[512]{};
static uint32_t g_mmioRegionCount=0;
static bool g_mmioMapValid=false;
// The bootloader's identity map is bounded to 128 GiB. Firmware-table reads
// additionally require reserved/ACPI memory from its captured memory map.
static constexpr uint64_t IdentityLimit=128ULL*1024*1024*1024;
static bool ContainsFirmware(uint64_t address,uint32_t bytes,bool nvsOnly)
{
    if(!bytes || address>=IdentityLimit || bytes>IdentityLimit-address)return false;
    const uint64_t end=address+bytes;uint64_t cursor=address;
    while(cursor<end) {
        uint64_t next=cursor;
        for(uint32_t i=0;i<g_regionCount;++i)
            if((!nvsOnly || g_regions[i].nvs) && cursor>=g_regions[i].begin && cursor<g_regions[i].end && g_regions[i].end>next)next=g_regions[i].end;
        if(next==cursor)return false;
        cursor=next<end?next:end;
    }
    return true;
}
static bool ReadFirmware(void*,uint64_t address,void* out,uint32_t bytes)
{
    if(!out || !ContainsFirmware(address,bytes,false))return false;
    __builtin_memcpy(out,reinterpret_cast<const void*>(address),bytes);return true;
}
static bool CaptureFirmwareRegions(const MemoryDescriptor* map,uint32_t count)
{
    g_regionCount=0;g_mmioRegionCount=0;g_mmioMapValid=false;
    if(!map || !count || count>4096)return false;
    g_mmioMapValid=true;
    for(uint32_t i=0;i<count;++i) {
        const auto& entry=map[i];
        if(entry.type!=MemoryType::Reserved && entry.type!=MemoryType::AcpiReclaimable && entry.type!=MemoryType::AcpiNvs && entry.type!=MemoryType::Mmio)continue;
        if(entry.pageCount>~uint64_t{0}/4096) {
            if(entry.type==MemoryType::Mmio){g_mmioMapValid=false;g_mmioRegionCount=0;continue;}
            return false;
        }
        const uint64_t bytes=entry.pageCount*4096;
        if(bytes>~uint64_t{0}-entry.physicalStart) {
            if(entry.type==MemoryType::Mmio){g_mmioMapValid=false;g_mmioRegionCount=0;continue;}
            return false;
        }
        if(!bytes || entry.physicalStart>=IdentityLimit)continue;
        const uint64_t end=entry.physicalStart+bytes;
        if(entry.type==MemoryType::Mmio) {
            if(!g_mmioMapValid)continue;
            if(g_mmioRegionCount==sizeof(g_mmioRegions)/sizeof(g_mmioRegions[0])){g_mmioMapValid=false;g_mmioRegionCount=0;continue;}
            g_mmioRegions[g_mmioRegionCount++]={entry.physicalStart,end<IdentityLimit?end:IdentityLimit};continue;
        }
        if(g_regionCount==sizeof(g_regions)/sizeof(g_regions[0]))return false;
        g_regions[g_regionCount++]={entry.physicalStart,end<IdentityLimit?end:IdentityLimit,entry.type==MemoryType::AcpiNvs};
    }
    return g_regionCount!=0;
}
static bool ParseMadt(uint64_t physical,const AcpiSdtHeader& header)
{
    if(header.length<sizeof(MadtHeader))return false;
    // Find() checked the complete physical span and checksum before returning.
    const auto* madt=reinterpret_cast<const MadtHeader*>(physical);
    g_madt.localApicPhysical=madt->localApicAddress;
    const uint8_t* entry=reinterpret_cast<const uint8_t*>(madt)+sizeof(MadtHeader);
    const uint8_t* end=reinterpret_cast<const uint8_t*>(madt)+header.length;
    AcpiSciRoute sci{bool(g_legacyPower.sci),true,g_legacyPower.sci};
    bool sciOverride=false;
    while(entry<end) {
        if(uint64_t(end-entry)<sizeof(MadtEntryHeader))return false;
        const auto* item=reinterpret_cast<const MadtEntryHeader*>(entry);
        if(item->length<sizeof(MadtEntryHeader) || item->length>uint64_t(end-entry))return false;
        if(item->type==MADT_TYPE_LAPIC) {
            if(item->length<sizeof(MadtLapicEntry))return false;
            const auto* cpu=reinterpret_cast<const MadtLapicEntry*>(item);
            if((cpu->flags&1) && g_madt.processorCount<ACPI_MAX_PROCESSORS) {
                const auto i=g_madt.processorCount++;g_madt.apicIds[i]=cpu->apicId;g_madt.processorIds[i]=cpu->processorId;
            }
        } else if(item->type==MADT_TYPE_IOAPIC) {
            if(item->length<sizeof(MadtIoApicEntry))return false;
            if(!g_madt.ioApicPhysical) {
                const auto* io=reinterpret_cast<const MadtIoApicEntry*>(item);
                g_madt.ioApicPhysical=io->ioApicAddress;g_madt.ioApicGsiBase=io->gsiBase;
            }
        } else if(item->type==2) { // Interrupt Source Override
            if(item->length<10)return false;
            if(g_legacyPower.sci && entry[2]==0 && entry[3]==g_legacyPower.sci) {
                uint32_t gsi=0;
                uint16_t flags=0;
                __builtin_memcpy(&gsi,entry+4,sizeof(gsi));
                __builtin_memcpy(&flags,entry+8,sizeof(flags));
                const uint16_t polarity=flags&3;
                const uint16_t trigger=(flags>>2)&3;
                if(sciOverride || (flags&~0xfu) || polarity==2 || (trigger!=0 && trigger!=3))sci.available=false;
                sci.gsi=gsi;
                sci.activeLow=polarity!=1;
                sciOverride=true;
            }
        } else if(item->type==MADT_TYPE_LAPIC_OVERRIDE) {
            if(item->length<sizeof(MadtLapicOverride))return false;
            g_madt.localApicPhysical=reinterpret_cast<const MadtLapicOverride*>(item)->lapicAddress;
        }
        entry+=item->length;
    }
    g_sciRoute=sci;
    SerialPrintf("ACPI: MADT — %u processor(s), LAPIC @ 0x%p, IOAPIC @ 0x%p\n",g_madt.processorCount,
        reinterpret_cast<void*>(g_madt.localApicPhysical),reinterpret_cast<void*>(g_madt.ioApicPhysical));
    return true;
}
bool AcpiInit(uint64_t rsdp,const MemoryDescriptor* map,uint32_t count)
{
    g_madt={};g_reset={};g_pmTimer={};g_tables={};g_acpiReady=false;g_bootResourceMap.Capture(map,count);
    g_legacyPower={};g_sciRoute={};
    if(!CaptureFirmwareRegions(map,count) || !g_tables.Init(ReadFirmware,nullptr,rsdp)) {
        g_mmioMapValid=false;SerialPuts("ACPI: invalid/unreadable RSDP or root table\n");return false;
    }
    g_reset=g_tables.Reset();
    g_pmTimer=g_tables.PmTimer();
    auto legacy=g_tables.LegacyPower();
    if(legacy.facs && ContainsFirmware(legacy.facs,64,true)) {
        char signature[4]{};
        uint32_t bytes=0;
        if(g_tables.Copy(legacy.facs,signature,4) && signature[0]=='F' && signature[1]=='A' &&
           signature[2]=='C' && signature[3]=='S' && g_tables.Copy(legacy.facs+4,&bytes,4) &&
           bytes>=64 && bytes<=4096 && ContainsFirmware(legacy.facs,bytes,true))g_legacyPower=legacy;
    }
    SerialPrintf("ACPI: reset available=%u port=0x%x value=0x%x reason=%s\n",g_reset.available?1u:0u,g_reset.port,g_reset.value,g_reset.reason);
    SerialPrintf("ACPI: PM timer available=%u port=0x%x width=%u\n",g_pmTimer.available?1u:0u,g_pmTimer.port,g_pmTimer.wide?32u:24u);
    AcpiSdtHeader header{};const auto madt=g_tables.Find("APIC",&header);
    if(!madt || !ParseMadt(madt,header)) {
        g_madt={};g_legacyPower={};g_sciRoute={};g_mmioMapValid=false;
        SerialPuts("ACPI: MADT missing or malformed\n");return false;
    }
    if(g_sciRoute.available)
        SerialPrintf("ACPI: legacy SCI irq=%u gsi=%u polarity=%s trigger=level\n",
            g_legacyPower.sci,g_sciRoute.gsi,g_sciRoute.activeLow?"low":"high");
#ifdef BROOK_DELL_ACPI_MONITOR
    g_rsdp=rsdp;
#endif
    g_acpiReady=true;return true;
}
const MadtInfo& AcpiGetMadt(){return g_madt;}
const AcpiResetInfo& AcpiGetResetInfo(){return g_reset;}
const AcpiPmTimerInfo& AcpiGetPmTimerInfo(){return g_pmTimer;}
const AcpiLegacyPowerInfo& AcpiGetLegacyPowerInfo(){return g_legacyPower;}
const AcpiSciRoute& AcpiGetSciRoute(){return g_sciRoute;}
#ifdef BROOK_DELL_ACPI_MONITOR
uint64_t AcpiAdmitDellMonitorFirmware() {
    // Archived XPS 9343 FADT, MADT, DSDT and 11 SSDTs in root order.
    constexpr uint8_t expected[32]={
        0x0f,0x25,0x88,0xec,0x24,0x29,0x7e,0xd3,0x42,0xfd,0xe5,0x96,0x3e,0x44,0xaf,0x3e,
        0x6b,0x01,0x7e,0xe0,0xa1,0x58,0x1d,0x46,0xe8,0xd7,0xcb,0x2e,0x28,0xdb,0xb6,0xf4,
    };
    if(!g_acpiReady || !g_legacyPower.facs || !g_sciRoute.available ||
       !g_pmTimer.available || g_tables.RootEntryCount()>64)return 0;
    BrookSha256 hash;
    BrookShaInit(&hash);
    auto add=[&](uint64_t address,const AcpiSdtHeader& h) {
        if(!address || !ContainsFirmware(address,h.length,false))return false;
        BrookShaAdd(&hash,reinterpret_cast<const uint8_t*>(address),h.length);
        return true;
    };
    AcpiSdtHeader header{};
    const uint64_t fadt=g_tables.Find("FACP",&header);
    if(!add(fadt,header))return 0;
    const uint64_t madt=g_tables.Find("APIC",&header);
    if(!add(madt,header))return 0;
    const uint64_t dsdt=g_tables.Dsdt(&header);
    if(!add(dsdt,header))return 0;
    unsigned ssdts=0,fadts=0,madts=0;
    for(uint32_t i=0;i<g_tables.RootEntryCount();++i) {
        const uint64_t address=g_tables.RootEntry(i,&header);
        if(!address)return 0;
        const char* sig=header.signature;
        if(memcmp(sig,"FACP",4)==0)++fadts;
        if(memcmp(sig,"APIC",4)==0)++madts;
        if(memcmp(sig,"PSDT",4)==0 || memcmp(sig,"DSDT",4)==0)return 0;
        if(memcmp(sig,"SSDT",4)==0) {
            if(++ssdts>11 || !add(address,header))return 0;
        }
    }
    uint8_t actual[32]{};
    BrookShaFinish(&hash,actual);
    return ssdts==11 && fadts==1 && madts==1 && BrookUpdateEqual(actual,expected,32)?g_rsdp:0;
}
bool AcpiFirmwareRange(uint64_t address,uint32_t bytes) {
    return g_acpiReady && ContainsFirmware(address,bytes,false);
}
#endif
uint64_t AcpiFindTable(const char* signature){return g_tables.Find(signature);}
bool AcpiReadBootPerformance(FpdtBootRecord& out) {
    out={};
    if(!g_acpiReady)return false;
    AcpiSdtHeader header{};
    const uint64_t fpdt=g_tables.Find("FPDT",&header);
    // Find() checked the complete span and checksum.
    if(!fpdt || header.length>4096)return false;
    const uint64_t fbpt=BootFpdtFbptAddress(reinterpret_cast<const uint8_t*>(fpdt),header.length);
    uint8_t head[8]{};
    if(!fbpt || !ReadFirmware(nullptr,fbpt,head,sizeof(head)))return false;
    const uint32_t length=BootLe32(head+4);
    static uint8_t table[1024];
    if(length<8 || length>sizeof(table) || !ReadFirmware(nullptr,fbpt,table,length))return false;
    return BootParseFbpt(table,length,out);
}
bool AcpiNvsContains(uint64_t address,uint32_t bytes){return ContainsFirmware(address,bytes,true);}
bool AcpiMmioMapValid(){return g_mmioMapValid;}
bool AcpiClassifyBootRange(uint64_t address,uint32_t bytes,BootResourceClassification& result) {
    if(!g_acpiReady){result={};return false;}
    return g_bootResourceMap.Classify(address,bytes,result);
}
bool AcpiMmioContains(uint64_t address,uint32_t bytes) {
    if(!g_mmioMapValid || !bytes || address>=IdentityLimit || bytes>IdentityLimit-address)return false;
    const uint64_t end=address+bytes;uint64_t cursor=address;
    while(cursor<end) {
        uint64_t next=cursor;
        for(uint32_t i=0;i<g_mmioRegionCount;++i)
            if(cursor>=g_mmioRegions[i].begin && cursor<g_mmioRegions[i].end && g_mmioRegions[i].end>next)next=g_mmioRegions[i].end;
        if(next==cursor)return false;cursor=next<end?next:end;
    }
    return true;
}
bool AcpiNvsRead(uint64_t address,void* out,uint32_t bytes) {
    if(!out || bytes>8 || !AcpiNvsContains(address,bytes))return false;
    const auto* source=reinterpret_cast<const volatile uint8_t*>(address);auto* target=static_cast<uint8_t*>(out);
    for(uint32_t i=0;i<bytes;++i)target[i]=source[i];return true;
}
uint32_t AcpiInventoryCount(){return g_tables.Initialized()?g_tables.RootEntryCount()+1:0;}
bool AcpiUniqueDmarCopy(void* out,uint32_t capacity,uint32_t& bytes) {
    bytes=0;if(!out || !capacity || !g_tables.Initialized())return false;
    uint64_t found=0;AcpiSdtHeader selected{};
    for(uint32_t i=0;i<g_tables.RootEntryCount();++i) {
        AcpiSdtHeader h{};const uint64_t address=g_tables.RootEntry(i,&h);
        if(!address)return false;
        if(h.signature[0]=='D' && h.signature[1]=='M' && h.signature[2]=='A' && h.signature[3]=='R') {
            if(found)return false;found=address;selected=h;
        }
    }
    if(!found || selected.length>capacity || !g_tables.CopyTable(found,0,out,selected.length))return false;
    bytes=selected.length;return true;
}
bool AcpiInventoryRead(uint32_t index,uint32_t offset,void* out,uint32_t bytes,AcpiSdtHeader& header) {
    if(index>=AcpiInventoryCount() || bytes>512 || (bytes && !out))return false;
    const uint64_t address=index?g_tables.RootEntry(index-1,&header):g_tables.Dsdt(&header);
    if(!address || offset>header.length || bytes>header.length-offset)return false;
    // Firmware tables such as MSDM can contain product keys. Expose only AML
    // and fixed DMA-remapping/watchdog description tables, never arbitrary
    // ACPI tables. Copying WDAT does not execute its register instructions.
    const char* s=header.signature;
    if(!((s[0]=='D' && s[1]=='S' && s[2]=='D' && s[3]=='T') ||
         (s[0]=='S' && s[1]=='S' && s[2]=='D' && s[3]=='T') ||
         (s[0]=='D' && s[1]=='M' && s[2]=='A' && s[3]=='R') ||
         (s[0]=='W' && s[1]=='D' && s[2]=='A' && s[3]=='T')))return false;
    return !bytes || g_tables.CopyTable(address,offset,out,bytes);
}
} // namespace brook
