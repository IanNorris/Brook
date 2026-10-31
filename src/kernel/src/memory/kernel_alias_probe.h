#pragma once
#include <stdint.h>
namespace brook {
struct KernelAliasMapping {
    bool present=false;
    uint8_t pageShift=0,patIndex=0;
    uint64_t physical=0;
};
// Read-only page-table walk. Io must only read trusted kernel page-table pages;
// no device memory is accessed. PAT index is not the effective memory type:
// MTRRs, PAT MSR values, CPU state and other aliases remain separate concerns.
template<class Io> bool InspectKernelAlias(Io& io,uint64_t root,uint64_t address,KernelAliasMapping& result) {
    result={};constexpr uint64_t mask=0x000ffffffffff000ull;
    if(!root || (root&4095) || root>=1ull<<34)return false;
    const uint64_t upper=address>>48;
    if(upper!=((address&(1ull<<47))?0xffffull:0))return false;
    uint64_t table=root;
    for(unsigned level=4;level;--level) {
        const unsigned shift=12+9*(level-1);uint64_t entry=0;
        if(!io.readEntry(table,uint16_t((address>>shift)&511),entry))return false;
        if(!(entry&1))return true;
        const bool large=(entry&(1ull<<7)) && (level==3 || level==2);
        if(level==4 && (entry&(1ull<<7)))return false;
        if(large || level==1) {
            const unsigned pageShift=large?shift:12;const uint64_t pageMask=(1ull<<pageShift)-1;
            // Huge-page PAT occupies bit 12; other low address bits must be zero.
            if(large && (entry&mask&pageMask&~(1ull<<12)))return false;
            result.present=true;result.pageShift=uint8_t(pageShift);
            result.patIndex=uint8_t((((entry>>(large?12:7))&1)<<2)|(((entry>>4)&1)<<1)|((entry>>3)&1));
            result.physical=(entry&mask&~pageMask)|(address&pageMask);return true;
        }
        table=entry&mask;
        // All Brook page tables must lie inside the 16 GiB direct map.
        if(!table || table>=1ull<<34)return false;
    }
    return false;
}
}
