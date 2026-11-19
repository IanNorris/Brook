#pragma once
#include "core_dump_elf.h"

namespace brook {

enum CorePagePriority : unsigned {
    CoreStackHeap, CoreOtherData, CoreReadOnlyFile, CorePagePriorities
};
struct CorePageRef {
    uint64_t address, identity;
    uint32_t flags;
};
struct CorePageRange {
    uint32_t begin, end;
};

// Pages are collected in priority order, with each priority walking the
// address space in ascending order. Merge these runs for on-disk order.
class CorePageCursor {
    const CorePageRef* pages;
    CorePageRange ranges[CorePagePriorities];
public:
    CorePageCursor(const CorePageRef* pages,const CorePageRange* runs):pages(pages) {
        for(unsigned p=0;p<CorePagePriorities;++p)ranges[p]=runs[p];
    }
    bool next(CorePageRef& out) {
        unsigned best=CorePagePriorities;
        for(unsigned p=0;p<CorePagePriorities;++p) {
            if(ranges[p].begin==ranges[p].end)continue;
            if(best==CorePagePriorities || pages[ranges[p].begin].address<pages[ranges[best].begin].address)
                best=p;
        }
        if(best==CorePagePriorities)return false;
        out=pages[ranges[best].begin++];
        return true;
    }
};

inline uint32_t CoreBuildLoads(CorePageCursor pages,CoreProgramHeader* programs)
{
    CorePageRef page{};
    CoreProgramHeader load{};
    uint32_t count=1; // PT_NOTE
    while(pages.next(page)){
        if(load.memorySize && (load.flags!=page.flags || load.address+load.memorySize!=page.address)){
            if(programs)programs[count]=load;
            ++count;load={};
        }
        if(!load.memorySize)load={1,page.flags,0,page.address,0,0,0,CorePageSize};
        load.fileSize+=CorePageSize;
        load.memorySize+=CorePageSize;
    }
    if(load.memorySize){
        if(programs)programs[count]=load;
        ++count;
    }
    return count;
}

// Charge for one segment per page even if adjacent pages ultimately merge.
// A full page and all its worst-case metadata can always be committed.
inline uint32_t CoreLoadPageBudget(uint64_t byteLimit,uint64_t noteBytes)
{
    const uint64_t metadata=CorePageAlign(CoreHeaderBytes(1)+sizeof(CoreSectionHeader)+noteBytes);
    const uint64_t usable=byteLimit&~(CorePageSize-1);
    if(metadata>=usable)return 0;
    const uint64_t pages=(usable-metadata)/(CorePageSize+sizeof(CoreProgramHeader));
    constexpr uint32_t maxCount=~uint32_t{0};
    return pages<maxCount?static_cast<uint32_t>(pages):maxCount-1;
}
}
