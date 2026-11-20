#pragma once
#include "ahci_data_reader.h"
#include "ahci_write_target.h"

namespace brook {

inline bool AhciVmWriteSectorCount(const char* data, uint32_t length, uint64_t& sectors)
{
    constexpr char version[] = "BRKAHCIVM1\n";
    constexpr uint32_t prefix = sizeof(version) - 1;
    sectors = 0;
    if (!data || length <= prefix || length > prefix + 16 || memcmp(data, version, prefix)) return false;
    const char* end = data + length;
    const char* value = data + prefix;
    uint64_t count = 0;
    if (!AhciTargetNumber(value, end, count, '\n') || value != end || count < 68 || count > (1ull << 48))
        return false;
    sectors = count;
    return true;
}

template<class Cpuid>
bool AhciVmWritePlatform(Cpuid cpuid)
{
    uint32_t a, b, c, d;
    cpuid(1u, a, b, c, d);
    if (!(c & (1u << 31))) return false;
    cpuid(0x40000000u, a, b, c, d);
    if (a < 0x40000000u) return false;
    return (b == 0x61774d56u && c == 0x4d566572u && d == 0x65726177u) || // VMwareVMware
           (b == 0x4b4d564bu && c == 0x564b4d56u && d == 0x0000004du) || // KVMKVMKVM
           (b == 0x54474354u && c == 0x43544743u && d == 0x47435447u);   // TCGTCGTCGTCG
}

inline bool AhciVmWriteGeometry(uint64_t sectors, const AtaIdentifyInfo& identify, bool flushExt)
{
    return flushExt && sectors >= 68 && sectors <= (1ull << 48) && sectors == identify.sectors &&
           identify.logicalSectorBytes == 512 && identify.physicalSectorBytes == 512;
}

// The VM policy authorizes a guarded attempt only. A dirty ext2 header still
// identifies the disk, but the normal independent checker must repair it
// before desktop activation. Neither these reads nor success here grant a
// writable mount or bypass the boot-lifetime partition leases.
inline bool AhciVmWriteLayout(const AhciDataReadIo& io, const AhciBootSelector& boot,
                              const AhciDataSelector& data, uint64_t sectors, uint8_t* entries,
                              uint8_t* mirror, uint32_t entryBytes, uint8_t* gdt, uint32_t gdtBytes)
{
    AhciDataInspection result{};
    return AhciReadAndInspectBootData(io, boot, data, sectors, entries, mirror, entryBytes, gdt, gdtBytes, result)
           != AhciDataInspectionState::Invalid;
}
}
