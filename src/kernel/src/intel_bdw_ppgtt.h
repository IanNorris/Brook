#pragma once
#include <stdint.h>

namespace brook {
// Builds private 4-level Gen8 PPGTT pages. The caller owns and pins all
// backing and table pages, validates DMA/IOMMU access and PAT configuration,
// publishes the pages, and retains them until all GPU users have retired.
// This function never touches hardware or makes the image GPU-visible.
struct BdwPpgttPage { uint64_t dma; uint8_t* cpu; };
enum class BdwPpgttCache : uint8_t { WriteBack, Uncached };
struct BdwPpgttMapping {
    uint64_t address, dma;
    bool writable;
    BdwPpgttCache cache=BdwPpgttCache::WriteBack;
};

inline bool BdwBuildPpgttImage(const BdwPpgttPage (&scratch)[4],
                               const BdwPpgttPage* tables,uint32_t tableCount,
                               const BdwPpgttMapping* maps,uint32_t mapCount,bool sorted=false) {
    constexpr uint64_t gpuLimit=uint64_t{1}<<48, dmaLimit=uint64_t{1}<<39;
    constexpr uint32_t maxTables=128,maxUnsortedMappings=1024,maxSortedMappings=65536;
    if(!tables || !tableCount || tableCount>maxTables || !maps || !mapCount ||
       mapCount>(sorted?maxSortedMappings:maxUnsortedMappings))return false;
    auto pageValid=[&](const BdwPpgttPage& p) {
        return p.cpu && reinterpret_cast<uintptr_t>(p.cpu)<=~uintptr_t{0}-4095 &&
            !(reinterpret_cast<uintptr_t>(p.cpu)&4095) && p.dma && !(p.dma&4095) && p.dma<dmaLimit;
    };
    auto overlaps=[](uintptr_t a,uintptr_t size,uintptr_t b,uintptr_t other) {
        if(a>~uintptr_t{0}-size || b>~uintptr_t{0}-other)return true;
        return a<b+other && b<a+size;
    };
    auto page=[&](uint32_t i)->const BdwPpgttPage& {return i<4?scratch[i]:tables[i-4];};
    for(uint32_t i=0;i<tableCount+4;++i) {
        const auto& p=page(i);if(!pageValid(p))return false;
        const auto addr=reinterpret_cast<uintptr_t>(p.cpu);
        if(overlaps(addr,4096,reinterpret_cast<uintptr_t>(scratch),sizeof(scratch)) ||
           overlaps(addr,4096,reinterpret_cast<uintptr_t>(tables),sizeof(*tables)*tableCount) ||
           overlaps(addr,4096,reinterpret_cast<uintptr_t>(maps),sizeof(*maps)*mapCount))return false;
        for(uint32_t j=0;j<i;++j)if(p.dma==page(j).dma || p.cpu==page(j).cpu)return false;
    }
    uint32_t needed=1;
    for(uint32_t i=0;i<mapCount;++i) {
        const auto& m=maps[i];
        if((m.address&4095) || m.address>=gpuLimit || !m.dma || (m.dma&4095) || m.dma>=dmaLimit ||
           (m.cache!=BdwPpgttCache::WriteBack && m.cache!=BdwPpgttCache::Uncached))return false;
        for(uint32_t j=0;j<tableCount+4;++j)if(m.dma==page(j).dma)return false;
        bool l4=false,l3=false,l2=false;
        if(sorted && i) {
            // For a sorted set only the immediate predecessor can share a
            // prefix; strict ordering also excludes duplicate GPU addresses.
            const uint64_t previous=maps[i-1].address;
            if(m.address<=previous)return false;
            l4=(m.address>>39)==(previous>>39);
            l3=(m.address>>30)==(previous>>30);
            l2=(m.address>>21)==(previous>>21);
        } else if(!sorted)for(uint32_t j=0;j<i;++j) {
            if(m.address==maps[j].address)return false;
            if((m.address>>39)==(maps[j].address>>39))l4=true;
            if((m.address>>30)==(maps[j].address>>30))l3=true;
            if((m.address>>21)==(maps[j].address>>21))l2=true;
        }
        needed+=!l4;needed+=!l3;needed+=!l2;
        if(needed>tableCount)return false;
    }
    auto fill=[](const BdwPpgttPage& p,uint64_t value) {
        auto* words=reinterpret_cast<uint64_t*>(p.cpu);for(unsigned i=0;i<512;++i)words[i]=value;
    };
    // Scratch leaves are read-only; all higher-level entries are writable.
    fill(scratch[0],0);fill(scratch[1],scratch[0].dma|0x81);
    fill(scratch[2],scratch[1].dma|3);fill(scratch[3],scratch[2].dma|3);
    fill(tables[0],scratch[3].dma|3);uint32_t used=1;
    for(uint32_t i=0;i<mapCount;++i) {
        const auto& m=maps[i];uint32_t current=0;
        for(unsigned level=3;level>0;--level) {
            auto* words=reinterpret_cast<uint64_t*>(tables[current].cpu);
            const unsigned index=unsigned((m.address>>(12+9*level))&511);
            const uint64_t empty=scratch[level].dma|3;
            if(words[index]==empty) {
                const uint32_t next=used++;
                fill(tables[next],scratch[level-1].dma|(level==1?0x81:3));
                words[index]=tables[next].dma|3;current=next;
            } else {
                const uint64_t child=words[index]&~uint64_t{4095};
                uint32_t next=1;while(next<used && tables[next].dma!=child)++next;
                current=next;
            }
        }
        // The owner verifies private PAT index 4 (PAT) for WB and index
        // 3 (PCD | PWT) for UC before publishing these leaf mappings.
        const uint64_t cache=m.cache==BdwPpgttCache::Uncached?0x18:0x80;
        reinterpret_cast<uint64_t*>(tables[current].cpu)[(m.address>>12)&511]=m.dma|cache|(m.writable?3:1);
    }
    return used==needed;
}
}
