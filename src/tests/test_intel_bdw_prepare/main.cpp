#include "intel_bdw_scanout.h"
#include "memory/physical_memory.h"
#include <cassert>
#include <sys/mman.h>

static unsigned allocations=0,frees=0,failAt=0;
namespace brook {
PhysicalAddress PmmAllocPage(MemTag tag,uint16_t pid) {
    assert(tag==MemTag::KernelData && pid==KernelPid);
    if(++allocations==failAt)return PhysicalAddress(0);
    void* p=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    assert(p!=MAP_FAILED);return PhysicalAddress(reinterpret_cast<uintptr_t>(p));
}
void PmmFreePage(PhysicalAddress p){assert(!munmap(reinterpret_cast<void*>(p.raw()),4096));++frees;}
}
int main(){
    for(unsigned i=1;i<=19;++i){allocations=frees=0;failAt=i;assert(!brook::BdwPreparePrivateImages());assert(frees==i-1);}
    allocations=frees=0;failAt=0;assert(brook::BdwPreparePrivateImages());assert(allocations==19 && frees==19);
}
