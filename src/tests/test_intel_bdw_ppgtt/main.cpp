#include "../../kernel/src/intel_bdw_ppgtt.h"
#include "../../kernel/src/intel_bdw_exec_address_space.h"
#include <cassert>
#include <cstring>
using namespace brook;
alignas(4096) static uint8_t memory[20][4096];
static BdwPpgttMapping largeMaps[1025];
static uint64_t entry(const BdwPpgttPage& page,unsigned index) {
    return reinterpret_cast<const uint64_t*>(page.cpu)[index];
}
int main() {
    BdwPpgttPage scratch[4],tables[16];
    for(unsigned i=0;i<4;++i)scratch[i]={0x100000+4096ull*i,memory[i]};
    for(unsigned i=0;i<16;++i)tables[i]={0x200000+4096ull*i,memory[i+4]};
    const BdwPpgttMapping maps[]={{0x10000,0x400000,false},{0x11000,0x401000,true},
        {0x210000,0x402000,true},{0x40010000,0x403000,false},
        {(1ull<<39)+0x10000,0x404000,true}};
    assert(BdwBuildPpgttImage(scratch,tables,11,maps,5));
    auto lookup=[&](uint64_t address) {
        const BdwPpgttPage* current=&tables[0];
        for(unsigned level=3;level>0;--level) {
            uint64_t pte=entry(*current,(address>>(12+9*level))&511);assert((pte&3)==3);
            unsigned n=0;while(n<11 && tables[n].dma!=(pte&~uint64_t{4095}))++n;
            assert(n<11);current=&tables[n];
        }
        return entry(*current,(address>>12)&511);
    };
    for(const auto& map:maps)assert(lookup(map.address)==(map.dma|(map.writable?0x83:0x81)));
    assert(lookup(0x12000)==(scratch[0].dma|0x81));
    assert(entry(tables[0],2)==(scratch[3].dma|3));
    // Validation failures leave every supplied page untouched.
    memset(memory,0x5a,sizeof(memory));
    BdwPpgttMapping invalid[]={{0x1000,0x400000,true},{0x1000,0x401000,false}};
    assert(!BdwBuildPpgttImage(scratch,tables,16,invalid,2));
    for(const auto& page:memory)for(uint8_t b:page)assert(b==0x5a);
    invalid[1].address=1ull<<48;
    assert(!BdwBuildPpgttImage(scratch,tables,16,invalid,2));
    invalid[1]={0x2000,tables[1].dma,true};
    assert(!BdwBuildPpgttImage(scratch,tables,16,invalid,2));
    invalid[1]={0x2000,0x401000,true};
    assert(!BdwBuildPpgttImage(scratch,tables,3,invalid,2));
    for(const auto& page:memory)for(uint8_t b:page)assert(b==0x5a);
    for(unsigned i=0;i<1025;++i)largeMaps[i]={0x10000000ull+i*4096ull,0x1000000ull+i*4096ull,(i&1)!=0};
    assert(!BdwBuildPpgttImage(scratch,tables,16,largeMaps,1025));
    largeMaps[500].address=largeMaps[499].address;
    assert(!BdwBuildPpgttImage(scratch,tables,16,largeMaps,1025,true));
    for(const auto& page:memory)for(uint8_t b:page)assert(b==0x5a);
    largeMaps[500].address=0x10000000ull+500*4096ull;
    assert(BdwBuildPpgttImage(scratch,tables,6,largeMaps,1025,true));
    const auto* pdpt=&tables[1];const auto* pd=&tables[2];
    assert((entry(tables[0],0)&~4095ull)==pdpt->dma);
    assert((entry(*pdpt,0)&~4095ull)==pd->dma);
    for(unsigned i=0;i<1025;++i){const unsigned pdIndex=((largeMaps[i].address>>21)&511);
        const uint64_t ptDma=entry(*pd,pdIndex)&~4095ull;unsigned t=3;while(t<6 && tables[t].dma!=ptDma)++t;assert(t<6);
        assert(entry(tables[t],(largeMaps[i].address>>12)&511)==(largeMaps[i].dma|(largeMaps[i].writable?0x83:0x81)));}
    {const uint64_t first[]={0x500000,0x503000},second[]={0x700000};
     BdwExecMemoryObject objects[]={{0x10000,first,2,false},{0x210000,second,1,true}};
     BdwPpgttMapping workspace[3]{};
     assert(BdwBuildExecAddressSpace(scratch,tables,5,objects,2,workspace,3));
     assert(workspace[0].address==0x10000 && workspace[1].dma==0x503000 && !workspace[1].writable);
     assert(workspace[2].address==0x210000 && workspace[2].dma==0x700000 && workspace[2].writable);
     memset(memory,0x5a,sizeof(memory));objects[1].address=0x11000;
     assert(!BdwBuildExecAddressSpace(scratch,tables,5,objects,2,workspace,3));
     objects[1].address=1ull<<48;
     assert(!BdwBuildExecAddressSpace(scratch,tables,5,objects,2,workspace,3));
     objects[1].address=0x210000;
     assert(!BdwBuildExecAddressSpace(scratch,tables,5,objects,2,workspace,2));
     auto* overlapping=reinterpret_cast<BdwPpgttMapping*>(tables[0].cpu);
     assert(!BdwBuildExecAddressSpace(scratch,tables,5,objects,2,overlapping,3));
     for(const auto& page:memory)for(uint8_t b:page)assert(b==0x5a);}
    {
     uint64_t dma[5];BdwExecMemoryObject objects[5]{};BdwPpgttMapping workspace[5]{};
     for(unsigned i=0;i<5;++i){dma[i]=maps[i].dma;objects[i]={maps[i].address,&dma[i],1,maps[i].writable};}
     uint32_t count=99;assert(BdwExecAddressSpaceTableCount(objects,5,count) && count==10);
     assert(!BdwBuildExecAddressSpace(scratch,tables,count-1,objects,5,workspace,5));
     assert(BdwBuildExecAddressSpace(scratch,tables,count,objects,5,workspace,5));
     objects[1].address=objects[0].address;count=99;
     assert(!BdwExecAddressSpaceTableCount(objects,5,count) && count==99);
     objects[1].address=maps[1].address;objects[4].address=(1ull<<48)-4096;objects[4].pageCount=2;
     assert(!BdwExecAddressSpaceTableCount(objects,5,count) && count==99);
     uint64_t sparseDma[43]{};BdwExecMemoryObject sparse[43]{};
     for(unsigned i=0;i<43;++i){sparseDma[i]=0x800000+4096ull*i;sparse[i]={uint64_t(i)<<39,&sparseDma[i],1,false};}
     assert(!BdwExecAddressSpaceTableCount(sparse,43,count) && count==99);
     assert(BdwExecAddressSpaceTableCount(sparse,42,count) && count==127);
    }
}
