#pragma once
#include "efi_runtime_inventory.h"
#include "memory/kernel_alias_probe.h"
namespace brook {
struct EfiRuntimeMappingSummary {
    uint64_t checked=0,unavailable=0,wrongAddress=0,nonExecutable=0,nonWritable=0;
    bool complete=false;
};
// Read-only snapshot, not a firmware-call admission decision: PAT/MTRR, other
// aliases, CPU execution state and changes after the walk remain unverified.
template<class Probe> EfiRuntimeMappingSummary InspectEfiRuntimeMappings(
    const BootProtocol* boot,const EfiRuntimeInventory& inventory,Probe& probe) {
    EfiRuntimeMappingSummary out{};
    if(!inventory.valid || inventory.runtimePages>65536)return out;
    for(uint32_t i=0;i<boot->memoryMapCount;++i) {
        const auto& d=boot->memoryMap[i];if(!(d._reserved&BootMemoryEfiRuntime))continue;
        for(uint64_t page=0;page<d.pageCount;++page) {
            const uint64_t address=d.physicalStart+page*4096;KernelAliasMapping mapping{};
            ++out.checked;
            if(!probe(address,mapping) || !mapping.present){++out.unavailable;continue;}
            if(mapping.physical!=address)++out.wrongAddress;
            if((d._reserved&BootMemoryEfiRuntimeCode) && !mapping.executable)++out.nonExecutable;
            // EFI_MEMORY_RO in the memory map is a capability, not proof that
            // this runtime-data region may safely be read-only at call time.
            if((d._reserved&BootMemoryEfiRuntimeData) && !mapping.writable)++out.nonWritable;
        }
    }
    out.complete=out.checked==inventory.runtimePages;return out;
}
}
