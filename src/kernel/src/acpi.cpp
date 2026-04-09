#include "acpi.h"
#include "serial.h"

namespace brook {

// ---------------------------------------------------------------------------
// ACPI table header (common to all SDTs)
// ---------------------------------------------------------------------------

struct AcpiTableHeader
{
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oemId[6];
    char     oemTableId[8];
    uint32_t oemRevision;
    uint32_t creatorId;
    uint32_t creatorRevision;
} __attribute__((packed));

// RSDP (Root System Description Pointer) — ACPI 2.0 format
struct Rsdp
{
    char     signature[8];   // "RSD PTR "
    uint8_t  checksum;       // covers first 20 bytes
    char     oemId[6];
    uint8_t  revision;       // 0 = ACPI 1.0, 2 = ACPI 2.0+
    uint32_t rsdtAddress;    // physical address of RSDT (32-bit)
    // ACPI 2.0 extension:
    uint32_t length;
    uint64_t xsdtAddress;    // physical address of XSDT (64-bit)
    uint8_t  extChecksum;    // covers full structure
    uint8_t  reserved[3];
} __attribute__((packed));

// XSDT — array of 64-bit table pointers after the header
// RSDT — array of 32-bit table pointers after the header

// MADT (Multiple APIC Description Table)
struct MadtHeader
{
    AcpiTableHeader header;
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

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static MadtInfo  g_madt  = {};
static bool      g_ready = false;

// Physical address of XSDT (preferred) or RSDT.
static uint64_t  g_sdtPhysical   = 0;
static bool      g_useXsdt       = false;

// ---------------------------------------------------------------------------
// Checksum helpers
// ---------------------------------------------------------------------------

static bool VerifyChecksum(const void* data, uint32_t length)
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++) sum += p[i];
    return sum == 0;
}

static bool SigMatch(const char* a, const char* b, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return false;
    return true;
}

// ---------------------------------------------------------------------------
// MADT parser
// ---------------------------------------------------------------------------

static void ParseMadt(uint64_t madtPhys)
{
    const MadtHeader* madt = reinterpret_cast<const MadtHeader*>(madtPhys);

    // Set LAPIC address from MADT header (may be overridden by type-5 entry).
    g_madt.localApicPhysical = madt->localApicAddress;
    g_madt.ioApicPhysical    = 0;
    g_madt.ioApicGsiBase     = 0;
    g_madt.processorCount    = 0;

    const uint8_t* entry = reinterpret_cast<const uint8_t*>(madt) + sizeof(MadtHeader);
    const uint8_t* end   = reinterpret_cast<const uint8_t*>(madt) + madt->header.length;

    while (entry < end)
    {
        const MadtEntryHeader* hdr = reinterpret_cast<const MadtEntryHeader*>(entry);
        if (hdr->length < 2) break;  // sanity guard

        if (hdr->type == MADT_TYPE_LAPIC)
        {
            const MadtLapicEntry* lapic = reinterpret_cast<const MadtLapicEntry*>(entry);
            if ((lapic->flags & 1) && g_madt.processorCount < ACPI_MAX_PROCESSORS)
            {
                uint32_t i = g_madt.processorCount++;
                g_madt.apicIds[i]      = lapic->apicId;
                g_madt.processorIds[i] = lapic->processorId;
            }
        }
        else if (hdr->type == MADT_TYPE_IOAPIC && g_madt.ioApicPhysical == 0)
        {
            const MadtIoApicEntry* ioapic = reinterpret_cast<const MadtIoApicEntry*>(entry);
            g_madt.ioApicPhysical = ioapic->ioApicAddress;
            g_madt.ioApicGsiBase  = ioapic->gsiBase;
        }
        else if (hdr->type == MADT_TYPE_LAPIC_OVERRIDE)
        {
            const MadtLapicOverride* ovr = reinterpret_cast<const MadtLapicOverride*>(entry);
            g_madt.localApicPhysical = ovr->lapicAddress;
        }

        entry += hdr->length;
    }

    SerialPrintf("ACPI: MADT — %u processor(s), LAPIC @ 0x%p, IOAPIC @ 0x%p\n",
                 g_madt.processorCount,
                 reinterpret_cast<void*>(g_madt.localApicPhysical),
                 reinterpret_cast<void*>(g_madt.ioApicPhysical));
}

// ---------------------------------------------------------------------------
// SDT enumeration
// ---------------------------------------------------------------------------

// Get the number of table entries in the XSDT or RSDT.
static uint32_t SdtEntryCount()
{
    const AcpiTableHeader* hdr = reinterpret_cast<const AcpiTableHeader*>(g_sdtPhysical);
    uint32_t entryBytes = hdr->length - sizeof(AcpiTableHeader);
    return g_useXsdt ? (entryBytes / 8) : (entryBytes / 4);
}

// Get the physical address of SDT entry i.
static uint64_t SdtEntryPhys(uint32_t i)
{
    const uint8_t* base = reinterpret_cast<const uint8_t*>(g_sdtPhysical)
                        + sizeof(AcpiTableHeader);
    if (g_useXsdt)
    {
        uint64_t addr;
        __builtin_memcpy(&addr, base + i * 8, 8);
        return addr;
    }
    else
    {
        uint32_t addr;
        __builtin_memcpy(&addr, base + i * 4, 4);
        return static_cast<uint64_t>(addr);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool AcpiInit(uint64_t rsdpPhysical)
{
    if (rsdpPhysical == 0)
    {
        SerialPuts("ACPI: no RSDP in boot protocol\n");
        return false;
    }

    const Rsdp* rsdp = reinterpret_cast<const Rsdp*>(rsdpPhysical);

    // Verify RSDP signature.
    if (!SigMatch(rsdp->signature, "RSD PTR ", 8))
    {
        SerialPuts("ACPI: RSDP signature invalid\n");
        return false;
    }

    // Prefer XSDT (ACPI 2.0+) over RSDT.
    if (rsdp->revision >= 2 && rsdp->xsdtAddress != 0)
    {
        g_sdtPhysical = rsdp->xsdtAddress;
        g_useXsdt     = true;
        SerialPrintf("ACPI: XSDT @ 0x%p\n",
                     reinterpret_cast<void*>(g_sdtPhysical));
    }
    else
    {
        g_sdtPhysical = static_cast<uint64_t>(rsdp->rsdtAddress);
        g_useXsdt     = false;
        SerialPrintf("ACPI: RSDT @ 0x%p\n",
                     reinterpret_cast<void*>(g_sdtPhysical));
    }

    // Validate SDT header checksum.
    const AcpiTableHeader* sdtHdr =
        reinterpret_cast<const AcpiTableHeader*>(g_sdtPhysical);
    if (!VerifyChecksum(sdtHdr, sdtHdr->length))
    {
        SerialPuts("ACPI: SDT checksum failed\n");
        return false;
    }

    // Find and parse the MADT.
    uint64_t madtPhys = AcpiFindTable("APIC");  // MADT signature is "APIC"
    if (madtPhys == 0)
    {
        SerialPuts("ACPI: MADT (APIC table) not found\n");
        return false;
    }

    ParseMadt(madtPhys);
    g_ready = true;
    return true;
}

const MadtInfo& AcpiGetMadt()
{
    return g_madt;
}

uint64_t AcpiFindTable(const char* signature)
{
    if (g_sdtPhysical == 0) return 0;

    uint32_t count = SdtEntryCount();
    for (uint32_t i = 0; i < count; i++)
    {
        uint64_t tablePhys = SdtEntryPhys(i);
        if (tablePhys == 0) continue;

        const AcpiTableHeader* hdr =
            reinterpret_cast<const AcpiTableHeader*>(tablePhys);
        if (SigMatch(hdr->signature, signature, 4))
            return tablePhys;
    }
    return 0;
}

} // namespace brook
