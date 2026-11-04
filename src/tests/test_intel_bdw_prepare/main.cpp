#include "intel_bdw_scanout.h"
#include "memory/physical_memory.h"
#include <cassert>
#include <sys/mman.h>

static unsigned allocations=0,frees=0,failAt=0;
static bool disableFlush=false;
namespace brook {
bool BdwTestDisableClflush(){return disableFlush;}
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
    allocations=frees=0;failAt=0;disableFlush=true;assert(!brook::BdwPreparePrivateImages());assert(allocations==19 && frees==19);
    allocations=frees=0;disableFlush=false;
    uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
    const uint32_t line=((b>>8)&255)*8;
    const bool supported=(d&(1u<<19)) && line>=16 && line<=4096 && !(line&(line-1)) && !(4096%line);
    assert(brook::BdwPreparePrivateImages()==supported);assert(allocations==19 && frees==19);
}
