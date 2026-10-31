#include "acpi.h"
#include "acpi_tables.h"
#include "boot_resource_map.h"
#include "boot_protocol/boot_protocol.h"
#include "serial.h"

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
static AcpiTableReader g_tables;
static BootResourceMap g_bootResourceMap;
static bool g_acpiReady=false;
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
        } else if(item->type==MADT_TYPE_LAPIC_OVERRIDE) {
            if(item->length<sizeof(MadtLapicOverride))return false;
            g_madt.localApicPhysical=reinterpret_cast<const MadtLapicOverride*>(item)->lapicAddress;
        }
        entry+=item->length;
    }
    SerialPrintf("ACPI: MADT — %u processor(s), LAPIC @ 0x%p, IOAPIC @ 0x%p\n",g_madt.processorCount,
        reinterpret_cast<void*>(g_madt.localApicPhysical),reinterpret_cast<void*>(g_madt.ioApicPhysical));
    return true;
}
bool AcpiInit(uint64_t rsdp,const MemoryDescriptor* map,uint32_t count)
{
    g_madt={};g_reset={};g_tables={};g_acpiReady=false;g_bootResourceMap.Capture(map,count);
    if(!CaptureFirmwareRegions(map,count) || !g_tables.Init(ReadFirmware,nullptr,rsdp)) {
        g_mmioMapValid=false;SerialPuts("ACPI: invalid/unreadable RSDP or root table\n");return false;
    }
    g_reset=g_tables.Reset();
    SerialPrintf("ACPI: reset available=%u port=0x%x value=0x%x reason=%s\n",g_reset.available?1u:0u,g_reset.port,g_reset.value,g_reset.reason);
    AcpiSdtHeader header{};const auto madt=g_tables.Find("APIC",&header);
    if(!madt || !ParseMadt(madt,header)) {
        g_madt={};g_mmioMapValid=false;SerialPuts("ACPI: MADT missing or malformed\n");return false;
    }
    g_acpiReady=true;return true;
}
const MadtInfo& AcpiGetMadt(){return g_madt;}
const AcpiResetInfo& AcpiGetResetInfo(){return g_reset;}
uint64_t AcpiFindTable(const char* signature){return g_tables.Find(signature);}
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
bool AcpiInventoryRead(uint32_t index,uint32_t offset,void* out,uint32_t bytes,AcpiSdtHeader& header) {
    if(index>=AcpiInventoryCount() || bytes>512 || (bytes && !out))return false;
    const uint64_t address=index?g_tables.RootEntry(index-1,&header):g_tables.Dsdt(&header);
    if(!address || offset>header.length || bytes>header.length-offset)return false;
    // Firmware tables such as MSDM can contain product keys. The audio
    // investigation only needs AML, so do not expose arbitrary ACPI tables.
    const char* s=header.signature;
    if(!((s[0]=='D' && s[1]=='S' && s[2]=='D' && s[3]=='T') ||
         (s[0]=='S' && s[1]=='S' && s[2]=='D' && s[3]=='T')))return false;
    return !bytes || g_tables.CopyTable(address,offset,out,bytes);
}
} // namespace brook
