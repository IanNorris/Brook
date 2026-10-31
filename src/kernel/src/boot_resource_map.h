#pragma once
#include "boot_protocol/boot_protocol.h"

namespace brook {
struct BootResourceClassification {uint32_t types=0;bool unreported=false;};
static_assert(static_cast<uint32_t>(MemoryType::Initrd)<32,"Boot resource type mask requires fewer than 32 types");
// Read-only boot-time descriptors, retained only for diagnostics. This is not
// device ownership or MMIO access authorization and does not include dynamic
// device allocations, PCI BAR extents or cache-alias state.
class BootResourceMap {
    static constexpr uint32_t Capacity=4096;
    MemoryDescriptor entries_[Capacity]{};
    uint32_t count_=0;bool valid_=false;
public:
    bool Capture(const MemoryDescriptor* entries,uint32_t count) {
        count_=0;valid_=false;
        if(!entries || !count || count>Capacity)return false;
        for(uint32_t i=0;i<count;++i) {
            const auto& e=entries[i];const uint32_t type=static_cast<uint32_t>(e.type);
            if(type>static_cast<uint32_t>(MemoryType::Initrd) || (e.physicalStart&4095) || e.pageCount>~uint64_t{0}/4096 ||
               e.pageCount*4096>~uint64_t{0}-e.physicalStart)return false;
            entries_[count_++]=e;
        }
        valid_=true;return true;
    }
    bool Classify(uint64_t address,uint32_t bytes,BootResourceClassification& result) const {
        result={};if(!valid_ || !bytes || bytes>~uint64_t{0}-address)return false;
        const uint64_t end=address+bytes;
        for(uint32_t i=0;i<count_;++i) {
            const auto& e=entries_[i];const uint64_t hi=e.physicalStart+e.pageCount*4096;
            if(e.pageCount && e.physicalStart<end && hi>address)result.types|=1u<<static_cast<uint32_t>(e.type);
        }
        uint64_t cursor=address;
        while(cursor<end) {
            uint64_t next=cursor;
            for(uint32_t i=0;i<count_;++i) {
                const auto& e=entries_[i];const uint64_t hi=e.physicalStart+e.pageCount*4096;
                if(e.physicalStart<=cursor && hi>next)next=hi;
            }
            if(next==cursor){result.unreported=true;break;}
            cursor=next<end?next:end;
        }
        return true;
    }
};
}
