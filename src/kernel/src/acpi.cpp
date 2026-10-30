#include "acpi.h"
#include "acpi_tables.h"
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
struct FirmwareRegion {uint64_t begin,end;};
static FirmwareRegion g_regions[512]{};
static uint32_t g_regionCount=0;
// The bootloader's identity map is bounded to 128 GiB. Firmware-table reads
// additionally require reserved/ACPI memory from its captured memory map.
static constexpr uint64_t IdentityLimit=128ULL*1024*1024*1024;
static bool ReadFirmware(void*,uint64_t address,void* out,uint32_t bytes)
{
    if(!out || !bytes || address>=IdentityLimit || bytes>IdentityLimit-address)return false;
    const uint64_t end=address+bytes;uint64_t cursor=address;
    while(cursor<end) {
        uint64_t next=cursor;
        for(uint32_t i=0;i<g_regionCount;++i)
            if(cursor>=g_regions[i].begin && cursor<g_regions[i].end && g_regions[i].end>next)next=g_regions[i].end;
        if(next==cursor)return false;
        cursor=next<end?next:end;
    }
    __builtin_memcpy(out,reinterpret_cast<const void*>(address),bytes);return true;
}
static bool CaptureFirmwareRegions(const MemoryDescriptor* map,uint32_t count)
{
    g_regionCount=0;
    if(!map || !count || count>4096)return false;
    for(uint32_t i=0;i<count;++i) {
        const auto& entry=map[i];
        if(entry.type!=MemoryType::Reserved && entry.type!=MemoryType::AcpiReclaimable && entry.type!=MemoryType::AcpiNvs)continue;
        if(entry.pageCount>~uint64_t{0}/4096)return false;
        const uint64_t bytes=entry.pageCount*4096;
        if(bytes>~uint64_t{0}-entry.physicalStart)return false;
        if(!bytes || entry.physicalStart>=IdentityLimit)continue;
        if(g_regionCount==sizeof(g_regions)/sizeof(g_regions[0]))return false;
        const uint64_t end=entry.physicalStart+bytes;
        g_regions[g_regionCount++]={entry.physicalStart,end<IdentityLimit?end:IdentityLimit};
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
    g_madt={};g_reset={};g_tables={};
    if(!CaptureFirmwareRegions(map,count) || !g_tables.Init(ReadFirmware,nullptr,rsdp)) {
        SerialPuts("ACPI: invalid/unreadable RSDP or root table\n");return false;
    }
    g_reset=g_tables.Reset();
    SerialPrintf("ACPI: reset available=%u port=0x%x value=0x%x reason=%s\n",g_reset.available?1u:0u,g_reset.port,g_reset.value,g_reset.reason);
    AcpiSdtHeader header{};const auto madt=g_tables.Find("APIC",&header);
    if(!madt || !ParseMadt(madt,header)) {
        g_madt={};SerialPuts("ACPI: MADT missing or malformed\n");return false;
    }
    return true;
}
const MadtInfo& AcpiGetMadt(){return g_madt;}
const AcpiResetInfo& AcpiGetResetInfo(){return g_reset;}
uint64_t AcpiFindTable(const char* signature){return g_tables.Find(signature);}
} // namespace brook
