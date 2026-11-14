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

// A caller-owned, already populated last-level table (512 PTEs) that covers
// one 2 MiB-aligned GPU virtual interval. The builder links it from the page
// directory instead of rebuilding its entries. The caller pins and publishes
// it for as long as any image referencing it may be executed, and must not
// change it while such an image is live.
struct BdwPpgttLeaf { uint64_t address, dma; };
constexpr uint32_t BdwPpgttMaxLeaves=64;

// Populate persistent leaf tables for count pages starting at a 2 MiB-aligned
// address. pageOf(i) yields the DMA address of page i. Unused tail entries
// point at the caller's read-only scratch page. Never touches hardware.
template<class PageOf> bool BdwFillPpgttLeaves(uint64_t scratchDma,const BdwPpgttPage* tables,uint32_t tableCount,
                                              PageOf pageOf,uint32_t count,BdwPpgttCache cache,bool writable) {
    constexpr uint64_t dmaLimit=uint64_t{1}<<39;
    if(!tables || !count || tableCount!=(count+511)/512 || tableCount>BdwPpgttMaxLeaves ||
       !scratchDma || (scratchDma&4095) || scratchDma>=dmaLimit ||
       (cache!=BdwPpgttCache::WriteBack && cache!=BdwPpgttCache::Uncached))return false;
    for(uint32_t t=0;t<tableCount;++t){
        const auto& p=tables[t];
        if(!p.cpu || (reinterpret_cast<uintptr_t>(p.cpu)&4095) || !p.dma || (p.dma&4095) || p.dma>=dmaLimit ||
           p.dma==scratchDma)return false;
        for(uint32_t u=0;u<t;++u)if(tables[u].dma==p.dma || tables[u].cpu==p.cpu)return false;
    }
    for(uint32_t i=0;i<count;++i){const uint64_t dma=pageOf(i);
        if(!dma || (dma&4095) || dma>=dmaLimit || dma==scratchDma)return false;
        for(uint32_t t=0;t<tableCount;++t)if(tables[t].dma==dma)return false;
    }
    const uint64_t flags=(cache==BdwPpgttCache::Uncached?0x18:0x80)|(writable?3:1);
    for(uint32_t t=0;t<tableCount;++t){
        auto* words=reinterpret_cast<uint64_t*>(tables[t].cpu);
        for(uint32_t i=0;i<512;++i){const uint32_t index=t*512+i;
            words[i]=index<count?(pageOf(index)|flags):(scratchDma|0x81);}
    }
    return true;
}

inline bool BdwBuildPpgttImage(const BdwPpgttPage (&scratch)[4],
                               const BdwPpgttPage* tables,uint32_t tableCount,
                               const BdwPpgttMapping* maps,uint32_t mapCount,bool sorted=false,
                               const BdwPpgttLeaf* leaves=nullptr,uint32_t leafCount=0) {
    constexpr uint64_t gpuLimit=uint64_t{1}<<48, dmaLimit=uint64_t{1}<<39;
    constexpr uint32_t maxTables=128,maxUnsortedMappings=1024,maxSortedMappings=65536;
    if(!tables || !tableCount || tableCount>maxTables || !maps || !mapCount ||
       mapCount>(sorted?maxSortedMappings:maxUnsortedMappings) ||
       (leafCount && !leaves) || leafCount>BdwPpgttMaxLeaves)return false;
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
    // Is any mapping's address equal to value at this shift (shared prefix)?
    auto mapPrefix=[&](unsigned shift,uint64_t value) {
        if(!sorted){for(uint32_t i=0;i<mapCount;++i)if((maps[i].address>>shift)==value)return true;return false;}
        uint32_t lo=0,hi=mapCount;const uint64_t key=value<<shift;
        while(lo<hi){const uint32_t mid=lo+(hi-lo)/2;if(maps[mid].address<key)lo=mid+1;else hi=mid;}
        return lo<mapCount && (maps[lo].address>>shift)==value;
    };
    for(uint32_t i=0;i<leafCount;++i) {
        const auto& f=leaves[i];
        if((f.address&((uint64_t{1}<<21)-1)) || f.address>=gpuLimit || !f.dma || (f.dma&4095) || f.dma>=dmaLimit ||
           mapPrefix(21,f.address>>21))return false;
        for(uint32_t j=0;j<tableCount+4;++j)if(f.dma==page(j).dma)return false;
        for(uint32_t j=0;j<mapCount;++j)if(maps[j].dma==f.dma)return false;
        bool l4=mapPrefix(39,f.address>>39),l3=mapPrefix(30,f.address>>30);
        for(uint32_t j=0;j<i;++j) {
            if(leaves[j].address==f.address || leaves[j].dma==f.dma)return false;
            if((leaves[j].address>>39)==(f.address>>39))l4=true;
            if((leaves[j].address>>30)==(f.address>>30))l3=true;
        }
        needed+=!l4;needed+=!l3;
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
    for(uint32_t i=0;i<leafCount;++i) {
        const auto& f=leaves[i];uint32_t current=0;
        for(unsigned level=3;level>1;--level) {
            auto* words=reinterpret_cast<uint64_t*>(tables[current].cpu);
            const unsigned index=unsigned((f.address>>(12+9*level))&511);
            const uint64_t empty=scratch[level].dma|3;
            if(words[index]==empty) {
                const uint32_t next=used++;
                fill(tables[next],scratch[level-1].dma|3);
                words[index]=tables[next].dma|3;current=next;
            } else {
                const uint64_t child=words[index]&~uint64_t{4095};
                uint32_t next=1;while(next<used && tables[next].dma!=child)++next;
                if(next>=used)return false;
                current=next;
            }
        }
        auto* pd=reinterpret_cast<uint64_t*>(tables[current].cpu);
        const unsigned index=unsigned((f.address>>21)&511);
        if(pd[index]!=(scratch[1].dma|3))return false;
        pd[index]=f.dma|3;
    }
    return used==needed;
}
}
