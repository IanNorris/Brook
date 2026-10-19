#pragma once
#include <stdint.h>
namespace brook {
// Caller serializes access. Leases are never recycled, even when an extent is.
// Release is only legal after verified host UNMAP (and final VMA/TLB release).
template<uint32_t Slots = 2048>
class GpuApertureAllocator {
public:
    struct Extent { uint64_t offset, size, lease; };
private:
    Extent extents[Slots]{};
    uint32_t count = 0;
    uint64_t nextLease = 0;
public:
    bool Allocate(uint64_t bytes, uint64_t capacity, Extent& result) {
        if (!bytes || bytes > ~uint64_t{0}-4095 || (capacity&4095) || count==Slots ||
            nextLease==~uint64_t{0}) return false;
        const uint64_t size=(bytes+4095)&~uint64_t{4095};
        if (size>capacity) return false;
        uint64_t offset=0;
        uint32_t index=0;
        for (;index<count;++index) {
            if (extents[index].offset>=offset && size<=extents[index].offset-offset) break;
            offset=extents[index].offset+extents[index].size;
        }
        if (offset>capacity || size>capacity-offset) return false;
        for (uint32_t i=count;i>index;--i) extents[i]=extents[i-1];
        result=extents[index]={offset,size,++nextLease};++count;
        return true;
    }
    bool Release(uint64_t lease) {
        for (uint32_t i=0;i<count;++i) {
            if (extents[i].lease!=lease) continue;
            for (uint32_t j=i+1;j<count;++j) extents[j-1]=extents[j];
            --count;return true;
        }
        return false;
    }
    uint32_t Count() const { return count; }
    uint64_t Bytes() const {
        uint64_t total=0;for(uint32_t i=0;i<count;++i)total+=extents[i].size;return total;
    }
};
} // namespace brook
