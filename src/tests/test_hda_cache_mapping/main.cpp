#include "../../kernel/src/hda_cache_mapping.h"
#include <cassert>
using namespace brook;
int main(){
    constexpr uint64_t p=0x12345000;KernelAliasMapping mmio{true,12,3,p};
    assert(HdaCacheMmioMapping(p,mmio));mmio.patIndex=0;assert(!HdaCacheMmioMapping(p,mmio));
    mmio.patIndex=3;mmio.pageShift=21;assert(!HdaCacheMmioMapping(p,mmio));
    KernelAliasMapping id{true,21,0,p},direct{id};assert(HdaCacheDmaMappings(p,id,direct));
    id.present=false;assert(HdaCacheDmaMappings(p,id,direct));id.present=true;id.patIndex=3;assert(!HdaCacheDmaMappings(p,id,direct));
    id.patIndex=0;direct.patIndex=1;assert(!HdaCacheDmaMappings(p,id,direct));
    direct.patIndex=0;direct.physical+=4096;assert(!HdaCacheDmaMappings(p,id,direct));
}
