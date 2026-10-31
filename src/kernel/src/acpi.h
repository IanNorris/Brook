#pragma once

#include <stdint.h>
#include "acpi_tables.h"

// ACPI subsystem — parses RSDP → XSDT/RSDT → MADT without external libraries.
//
// Only the tables needed for LAPIC/IOAPIC bringup are parsed at this stage.
// Future: use ACPICA for full AML support (device enumeration, power management).
//
// All pointers are physical addresses mapped through the identity map.
// Must be called after VmmInit() but before ApicInit().

namespace brook {

// Maximum processors we track from the MADT.
static constexpr uint32_t ACPI_MAX_PROCESSORS = 64;

// Information extracted from the MADT (Multiple APIC Description Table).
struct MadtInfo
{
    uint64_t localApicPhysical;   // Physical address of LAPIC MMIO registers
    uint64_t ioApicPhysical;      // Physical address of first I/O APIC (0 if none)
    uint32_t ioApicGsiBase;       // GSI base for the I/O APIC

    uint32_t processorCount;                          // Number of enabled processors
    uint8_t  apicIds[ACPI_MAX_PROCESSORS];            // Local APIC IDs (in order)
    uint8_t  processorIds[ACPI_MAX_PROCESSORS];       // ACPI Processor IDs
};

// Initialise the ACPI subsystem from the RSDP physical address.
// Locates and parses the MADT; stores results in an internal structure.
// Returns true on success, false if RSDP is null or tables are corrupt.
struct MemoryDescriptor;
bool AcpiInit(uint64_t rsdpPhysical,const MemoryDescriptor* memoryMap,uint32_t memoryMapCount);
// Frozen descriptor only; discovery never writes hardware registers.
const AcpiResetInfo& AcpiGetResetInfo();

// Return parsed MADT information.  Valid only after AcpiInit() returns true.
const MadtInfo& AcpiGetMadt();

// Find a table by its 4-character signature (e.g. "HPET", "FACP").
// Returns the physical address of the table header, or 0 if not found/invalid. Tables have bounded lengths and checked checksums.
uint64_t AcpiFindTable(const char* signature);
// Read-only AML inventory: index 0 is DSDT; subsequent indices select only
// validated SSDT entries. No caller-supplied physical addresses.
uint32_t AcpiInventoryCount();
bool AcpiInventoryRead(uint32_t index,uint32_t offset,void* out,uint32_t bytes,AcpiSdtHeader& header);

} // namespace brook
