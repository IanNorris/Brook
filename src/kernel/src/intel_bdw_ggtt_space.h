#pragma once
#include <stdint.h>

namespace brook {
// Software reservation of GGTT virtual slots. The caller serializes access,
// owns the graphics device, and must independently check the device PTEs and
// preserve firmware/display mappings. A reservation alone is not a mapping,
// a DMA authorization, or proof that firmware will not use the same slots.
struct BdwGgttReservation { uint32_t first=0,count=0;uint64_t token=0; };
class BdwGgttSpace {
    static constexpr uint32_t Entries=1u<<20, First=1u<<16, MaxReservations=64;
    uint64_t bitmap_[Entries/64]{};
    BdwGgttReservation reservations_[MaxReservations]{};
    uint64_t nextToken_=1;
    uint32_t limit_=0;
    bool used(uint32_t index) const {return (bitmap_[index/64]>>(index%64))&1;}
    void mark(uint32_t first,uint32_t count,bool value) {
        for(uint32_t i=first;i<first+count;++i) {
            const uint64_t bit=uint64_t{1}<<(i%64);
            if(value)bitmap_[i/64]|=bit;else bitmap_[i/64]&=~bit;
        }
    }
public:
    BdwGgttSpace()=default;
    BdwGgttSpace(const BdwGgttSpace&)=delete;
    BdwGgttSpace& operator=(const BdwGgttSpace&)=delete;
    // Configure once from the device's captured GGMS, before reserving slots.
    bool configure(uint32_t entries) {
        if(limit_ || entries<=First || entries>Entries)return false;
        limit_=entries;return true;
    }
    bool configuredFor(uint32_t entries) const {return limit_ && limit_==entries;}
    uint32_t Limit() const {return limit_;}
    bool owns(const BdwGgttReservation& r) const {
        if(!r.token || !r.count)return false;
        for(const auto& current:reservations_)
            if(current.token==r.token && current.first==r.first && current.count==r.count)return true;
        return false;
    }
    bool reserve(uint32_t count,BdwGgttReservation& out) {
        if(!limit_ || !count || count>32 || !nextToken_)return false;
        unsigned entry=0;while(entry<MaxReservations && reservations_[entry].token)++entry;
        if(entry==MaxReservations)return false;
        uint32_t start=First,run=0;
        for(uint32_t i=First;i<limit_;++i) {
            // The current MMIO accessor admits exactly one 4 KiB PTE page.
            // Do not return a lease that it cannot bind atomically.
            if((i&511u)==0)run=0;
            if(used(i)){run=0;continue;}
            if(!run)start=i;
            if(++run==count) {
                BdwGgttReservation next{start,count,nextToken_++};
                mark(start,count,true);reservations_[entry]=next;out=next;return true;
            }
        }
        return false;
    }
    // Large scanout-sized range, in 4 KiB pages. This is deliberately separate
    // from reserve(): the existing hardware binder accepts only small leases
    // within one PTE page and must never be assumed to support this range.
    // Software exclusion does not prove hardware PTEs are free or bindable.
    bool reserveExtent(uint32_t count,uint32_t alignment,BdwGgttReservation& out) {
        if(!limit_ || !count || count>65536 || !alignment || alignment>65536 ||
           (alignment&(alignment-1)) || !nextToken_)return false;
        unsigned entry=0;while(entry<MaxReservations && reservations_[entry].token)++entry;
        if(entry==MaxReservations)return false;
        uint32_t start=0,run=0;
        for(uint32_t i=First;i<limit_;++i){
            if(used(i)){run=0;continue;}
            if(!run){if(i&(alignment-1))continue;start=i;}
            if(++run==count){
                BdwGgttReservation next{start,count,nextToken_++};
                mark(start,count,true);reservations_[entry]=next;out=next;return true;
            }
        }
        return false;
    }
    // Caller must establish that no hardware or CPU user retains the range,
    // and that any GPU PTE mutation was restored and invalidated before this.
    bool release(const BdwGgttReservation& r) {
        if(!owns(r))return false;
        for(auto& current:reservations_)if(current.token==r.token) {
            mark(current.first,current.count,false);current={};return true;
        }
        return false;
    }
};
// Shared software reservation space. Every caller must use the same driver
// serialization and the same device-ownership protocol.
BdwGgttSpace& BdwGlobalGgttSpace();
}
