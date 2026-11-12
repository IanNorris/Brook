#pragma once
#include "boot_protocol/boot_protocol.h"

namespace brook {
// Read-only admission for a later firmware runtime mapping. This does not
// establish mapping permissions, a firmware calling context, or permission to
// invoke an entrypoint. Older boot-protocol handoffs must never be read past
// their version-2 extent.
struct EfiRuntimeInventory {
    bool valid=false;
    uint64_t resetSystem=0;
    uint64_t runtimePages=0;
    uint32_t runtimeRanges=0;
};

inline EfiRuntimeInventory InspectEfiRuntime(const BootProtocol* boot) {
    EfiRuntimeInventory out{};
    if (!boot || boot->magic!=BootProtocolMagic || (boot->version<3 || boot->version>5) ||
        !boot->memoryMap || !boot->memoryMapCount || boot->memoryMapCount>4096 ||
        !boot->efiResetSystemPhysical) return out;
    const auto* map=boot->memoryMap;
    for (uint32_t i=0;i<boot->memoryMapCount;++i) {
        const auto& d=map[i];
        if (!(d._reserved&BootMemoryEfiRuntime)) continue;
        if (!d.pageCount || (d.physicalStart&4095) ||
            d.pageCount>(~uint64_t{0}-d.physicalStart)/4096 ||
            (d.type!=MemoryType::Reserved && d.type!=MemoryType::Mmio) ||
            ((d._reserved&BootMemoryEfiRuntimeCode) && (d._reserved&BootMemoryEfiRuntimeData))) return {};
        const uint64_t end=d.physicalStart+d.pageCount*4096;
        for (uint32_t j=0;j<boot->memoryMapCount;++j) {
            if (i==j) continue;
            const auto& other=map[j];
            if (!other.pageCount || (other.physicalStart&4095) ||
                other.pageCount>(~uint64_t{0}-other.physicalStart)/4096) return {};
            const uint64_t otherEnd=other.physicalStart+other.pageCount*4096;
            if (d.physicalStart<otherEnd && other.physicalStart<end) return {};
        }
        if (out.runtimePages>~uint64_t{0}-d.pageCount) return {};
        out.runtimePages+=d.pageCount;
        ++out.runtimeRanges;
    }
    if (!out.runtimeRanges || !BootEfiRuntimeCodeContains(map,boot->memoryMapCount,boot->efiResetSystemPhysical)) return {};
    out.valid=true;out.resetSystem=boot->efiResetSystemPhysical;
    return out;
}
}
