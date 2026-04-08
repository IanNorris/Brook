#include "acpi.h"
#include "Guid/Acpi.h"

namespace brook
{
namespace bootloader
{

static bool CompareGuid(const EFI_GUID& a, const EFI_GUID& b)
{
    if (a.Data1 != b.Data1 || a.Data2 != b.Data2 || a.Data3 != b.Data3)
    {
        return false;
    }

    for (int i = 0; i < 8; i++)
    {
        if (a.Data4[i] != b.Data4[i])
        {
            return false;
        }
    }

    return true;
}

bool AcpiInit(EFI_SYSTEM_TABLE* systemTable, brook::AcpiInfo& outAcpi)
{
    EFI_GUID acpi20Guid = EFI_ACPI_20_TABLE_GUID;

    for (UINTN i = 0; i < systemTable->NumberOfTableEntries; i++)
    {
        EFI_CONFIGURATION_TABLE& entry = systemTable->ConfigurationTable[i];

        if (CompareGuid(entry.VendorGuid, acpi20Guid))
        {
            outAcpi.rsdpPhysical = (UINT64)entry.VendorTable;
            return true;
        }
    }

    return false;
}

} // namespace bootloader
} // namespace brook
