#pragma once
#include "intel_bdw_ppgtt.h"

namespace brook {
// Kernel-owned snapshot of one pinned buffer's backing. This is not the
// userspace i915 UAPI: the caller must copy and validate handles, hold every
// page and object lease, normalize 48-bit GPU addresses, serialize changes,
// and keep mappings alive until engine retirement or recovery. Objects must
// be sorted by GPU address and may not overlap. No GPU state is touched.
struct BdwExecMemoryObject {
    uint64_t address;
    const uint64_t* pageDma;
    uint32_t pageCount;
    bool writable;
};
// Count the root and distinct intermediate-table prefixes without expanding
// objects into one entry per page. Objects must already be sorted and pinned.
// The result sizes an allocation; the builder still validates every DMA page.
inline bool BdwExecAddressSpaceTableCount(const BdwExecMemoryObject* objects,uint32_t count,uint32_t& out) {
    constexpr uint64_t limit=uint64_t{1}<<48;
    if(!objects || !count || count>65536)return false;
    uint64_t needed=1,priorEnd=0;bool prior=false;
    for(uint32_t i=0;i<count;++i){
        const auto& o=objects[i];const uint64_t bytes=uint64_t(o.pageCount)*4096;
        if(!o.pageDma || !o.pageCount || (o.address&4095) || o.address>=limit || bytes>limit-o.address ||
           (prior && o.address<priorEnd))return false;
        const uint64_t last=o.address+bytes-1;
        constexpr unsigned shifts[]={39,30,21};
        for(unsigned shift:shifts) {
            const uint64_t firstPrefix=o.address>>shift,lastPrefix=last>>shift;
            needed+=lastPrefix-firstPrefix+1;
            if(prior && ((priorEnd-1)>>shift)==firstPrefix)--needed;
        }
        if(needed>128)return false;
        priorEnd=o.address+bytes;prior=true;
    }
    out=static_cast<uint32_t>(needed);return true;
}
inline bool BdwBuildExecAddressSpace(const BdwPpgttPage (&scratch)[4],
                                     const BdwPpgttPage* tables,uint32_t tableCount,
                                     const BdwExecMemoryObject* objects,uint32_t objectCount,
                                     BdwPpgttMapping* workspace,uint32_t workspaceCount) {
    constexpr uint64_t limit=uint64_t{1}<<48,dmaLimit=uint64_t{1}<<39;
    if(!objects || !objectCount || objectCount>65536 || !workspace || !workspaceCount || workspaceCount>65536 ||
       !tables || !tableCount || tableCount>128)return false;
    auto overlaps=[](uintptr_t a,uint64_t bytes,uintptr_t b,uint64_t other){
        return bytes>~uintptr_t{0}-a || other>~uintptr_t{0}-b || (a<b+other && b<a+bytes);
    };
    const uintptr_t work=reinterpret_cast<uintptr_t>(workspace);
    const uint64_t workBytes=uint64_t(workspaceCount)*sizeof(*workspace);
    if(overlaps(work,workBytes,reinterpret_cast<uintptr_t>(objects),uint64_t(objectCount)*sizeof(*objects)) ||
       overlaps(work,workBytes,reinterpret_cast<uintptr_t>(scratch),sizeof(scratch)) ||
       overlaps(work,workBytes,reinterpret_cast<uintptr_t>(tables),uint64_t(tableCount)*sizeof(*tables)))return false;
    for(unsigned i=0;i<4;++i)if(overlaps(work,workBytes,reinterpret_cast<uintptr_t>(scratch[i].cpu),4096))return false;
    for(uint32_t i=0;i<tableCount;++i)if(overlaps(work,workBytes,reinterpret_cast<uintptr_t>(tables[i].cpu),4096))return false;
    uint32_t total=0;uint64_t priorEnd=0;
    for(uint32_t i=0;i<objectCount;++i){const auto& o=objects[i];
        const uint64_t bytes=uint64_t(o.pageCount)*4096;
        if(!o.pageDma || !o.pageCount || o.pageCount>workspaceCount-total || (o.address&4095) ||
           o.address>=limit || bytes>limit-o.address || (i && o.address<priorEnd) ||
           overlaps(work,workBytes,reinterpret_cast<uintptr_t>(o.pageDma),uint64_t(o.pageCount)*sizeof(uint64_t)))return false;
        for(uint32_t j=0;j<o.pageCount;++j)if(!o.pageDma[j] || (o.pageDma[j]&4095) || o.pageDma[j]>=dmaLimit)return false;
        total+=o.pageCount;priorEnd=o.address+bytes;
    }
    uint32_t n=0;
    for(uint32_t i=0;i<objectCount;++i)for(uint32_t j=0;j<objects[i].pageCount;++j)
        workspace[n++]={objects[i].address+uint64_t(j)*4096,objects[i].pageDma[j],objects[i].writable};
    return BdwBuildPpgttImage(scratch,tables,tableCount,workspace,n,true);
}
}
