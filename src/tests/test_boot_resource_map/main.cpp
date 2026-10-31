#include "../../kernel/src/boot_resource_map.h"
#include <cassert>
#include <cstdint>
using namespace brook;
static BootResourceMap inventory;
int main(){
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
