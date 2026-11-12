#include "../../kernel/src/boot_resource_map.h"
#include "../../kernel/src/efi_runtime_inventory.h"
#include "../../kernel/src/efi_runtime_mapping.h"
#include "boot_protocol/efi_runtime_cache.h"
#include "boot_protocol/efi_runtime_mtrr.h"
#include <cassert>
#include <cstdint>
using namespace brook;
static BootResourceMap inventory;
int main(){
    // Older loaders leave the extension zero. Runtime-marked ranges must not
    // be handed to the physical allocator, including anomalous Free ranges.
    constexpr auto ordinary=BootMemoryDescriptor(0x1000,1,MemoryType::Free,8,false,false);
    static_assert(ordinary.type==MemoryType::Free && ordinary._reserved==0);
    constexpr auto runtime=BootMemoryDescriptor(0x2000,2,MemoryType::Free,(1ull<<63)|8,true,false);
    static_assert(runtime.type==MemoryType::Reserved && runtime._reserved==(BootMemoryEfiRuntime|BootMemoryEfiRuntimeCode|8));
    constexpr auto runtimeData=BootMemoryDescriptor(0x4000,1,MemoryType::BootData,1ull<<63,false,true);
    static_assert(runtimeData.type==MemoryType::Reserved && (runtimeData._reserved&BootMemoryEfiRuntimeData));
    constexpr auto runtimeMmio=BootMemoryDescriptor(0x5000,1,MemoryType::Mmio,(1ull<<63)|1,false,false);
    static_assert(runtimeMmio.type==MemoryType::Mmio && (runtimeMmio._reserved&BootMemoryEfiRuntime));
    constexpr MemoryDescriptor runtimeMap[]={runtime,runtimeData,runtimeMmio};
    static_assert(BootEfiRuntimeCodeContains(runtimeMap,3,0x2000));
    static_assert(BootEfiRuntimeCodeContains(runtimeMap,3,0x3fff));
    static_assert(!BootEfiRuntimeCodeContains(runtimeMap,3,0x4000));
    static_assert(!BootEfiRuntimeCodeContains(runtimeMap,3,0x5000));
    static_assert(!BootEfiRuntimeCodeContains(runtimeMap,4097,0x2000));
    auto cacheProbe=[](uint64_t address,uint64_t& physical,uint8_t& pat){physical=address;pat=address==0x5000?3:0;return true;};
    auto cache=FingerprintRuntimeCache(runtimeMap,3,cacheProbe);
    assert(cache.valid && cache.pages==4 && cache.slots==9 && RuntimePatSlotsMatch(0x0000000000000006ull,0x0000000000000006ull,cache.slots));
    assert(!RuntimePatSlotsMatch(6,5,cache.slots));
    auto changedProbe=[](uint64_t address,uint64_t& physical,uint8_t& pat){physical=address;pat=0;return true;};
    assert(FingerprintRuntimeCache(runtimeMap,3,changedProbe).hash!=cache.hash);
    uint64_t base[32]{},mask[32]{},fixed[11]{};base[0]=0x100006;mask[0]=0xfffff800;
    const auto mtrr=RuntimeMtrrFingerprint(39,1,0x806,base,mask,fixed,1);assert(mtrr);
    mask[0]^=0x1000;assert(RuntimeMtrrFingerprint(39,1,0x806,base,mask,fixed,1)!=mtrr);
    assert(!RuntimeMtrrFingerprint(39,2,0x806,base,mask,fixed,1));
    BootProtocol protocol{};protocol.magic=BootProtocolMagic;protocol.version=2;
    protocol.memoryMap=const_cast<MemoryDescriptor*>(runtimeMap);protocol.memoryMapCount=3;
    protocol.efiResetSystemPhysical=0x2000;
    assert(!InspectEfiRuntime(&protocol).valid); // no version-2 extension read
    protocol.version=3;auto firmware=InspectEfiRuntime(&protocol);
    assert(firmware.valid && firmware.runtimeRanges==3 && firmware.runtimePages==4 && firmware.resetSystem==0x2000);
    auto good=[](uint64_t address,KernelAliasMapping& mapping){mapping.present=true;mapping.physical=address;mapping.executable=true;mapping.writable=true;return true;};
    auto mappings=InspectEfiRuntimeMappings(&protocol,firmware,good);
    assert(mappings.complete && mappings.checked==4 && !mappings.unavailable && !mappings.nonExecutable && !mappings.nonWritable);
    auto badMapping=[](uint64_t address,KernelAliasMapping& mapping){mapping.present=true;mapping.physical=address+4096;mapping.executable=false;mapping.writable=false;return true;};
    mappings=InspectEfiRuntimeMappings(&protocol,firmware,badMapping);
    assert(mappings.complete && mappings.wrongAddress==4 && mappings.nonExecutable==2 && mappings.nonWritable==1);
    MemoryDescriptor roCapability[]={runtime,runtimeData,runtimeMmio};roCapability[1]._reserved|=0x20000;
    protocol.memoryMap=roCapability;mappings=InspectEfiRuntimeMappings(&protocol,firmware,badMapping);
    assert(mappings.nonWritable==1); // capability is not current protection
    MemoryDescriptor bad[]={runtime,runtimeData,runtimeMmio};protocol.memoryMap=bad;
    bad[1].physicalStart=0x3000;assert(!InspectEfiRuntime(&protocol).valid); // overlap
    bad[1]=runtimeData;bad[0].type=MemoryType::Free;assert(!InspectEfiRuntime(&protocol).valid);
    bad[0]=runtime;bad[0].pageCount=~0ull;assert(!InspectEfiRuntime(&protocol).valid);
    bad[0]=runtime;bad[0]._reserved|=BootMemoryEfiRuntimeData;assert(!InspectEfiRuntime(&protocol).valid);
    bad[0]=runtime;protocol.efiResetSystemPhysical=0x4000;assert(!InspectEfiRuntime(&protocol).valid);
    BootResourceClassification result{};
    assert(!inventory.Classify(0x1000,4096,result));
    MemoryDescriptor map[]={
        {0x1000,1,MemoryType::Free,0},{0x2000,1,MemoryType::Reserved,0},
        {0x2000,1,MemoryType::AcpiNvs,0},{0x5000,1,MemoryType::Mmio,0}};
    assert(inventory.Capture(map,4));
    assert(inventory.Classify(0x2000,4096,result));
    assert(result.types==((1u<<uint32_t(MemoryType::Reserved))|(1u<<uint32_t(MemoryType::AcpiNvs))) && !result.unreported);
    assert(inventory.Classify(0x1800,4097,result));
    assert(result.types==((1u<<uint32_t(MemoryType::Free))|(1u<<uint32_t(MemoryType::Reserved))|(1u<<uint32_t(MemoryType::AcpiNvs))) && !result.unreported);
    assert(inventory.Classify(0x3000,0x3000,result) && result.unreported && (result.types&(1u<<uint32_t(MemoryType::Mmio))));
    assert(inventory.Classify(0x4000,4096,result) && result.unreported && result.types==0); // boundary contact is not overlap
    map[0].type=MemoryType::Mmio;assert(inventory.Classify(0x1000,4096,result) && result.types==(1u<<uint32_t(MemoryType::Free)));
    assert(!inventory.Classify(~0ull,2,result) && !inventory.Classify(0,0,result));
    map[0].type=static_cast<MemoryType>(999);assert(!inventory.Capture(map,4) && !inventory.Classify(0x1000,1,result));
    map[0].type=MemoryType::Free;map[0].pageCount=~0ull;assert(!inventory.Capture(map,4));
    map[0].pageCount=1;map[0].physicalStart=0x1001;assert(!inventory.Capture(map,4));
    map[0].pageCount=1;map[0].physicalStart=~0ull-100;assert(!inventory.Capture(map,4));
    assert(!inventory.Capture(nullptr,1) && !inventory.Capture(map,4097));
    map[0].physicalStart=0x1000;assert(inventory.Capture(map,4) && inventory.Classify(0x1000,4096,result));
}
