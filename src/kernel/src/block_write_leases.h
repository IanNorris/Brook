#pragma once
#include <stdint.h>

namespace brook {
// Pure state; caller serializes every operation with the same lock used for
// write admission, and pins parent/owner identities for the full lease life.
// A zero owner reserves metadata without permitting any leased writes.
struct BlockWriteLease { uintptr_t parent=0,owner=0;uint64_t start=0,length=0; };
class BlockWriteLeases {
public:
    static constexpr uint32_t Capacity=8;
private:
    BlockWriteLease leases_[Capacity]{};
    uint32_t count_=0;
    static bool valid(const BlockWriteLease& x) {
        return x.parent && x.length && x.start<=~uint64_t{0}-x.length;
    }
    static bool overlaps(const BlockWriteLease& a,const BlockWriteLease& b) {
        return a.parent==b.parent && a.start<b.start+b.length && b.start<a.start+a.length;
    }
public:
    // Publish the entire batch or none. Quiescence includes pending cached
    // writes, not just active I/O; the caller must establish both beforehand.
    bool Install(const BlockWriteLease* requested,uint32_t count,bool quiescent) {
        if(!quiescent || !requested || !count || count>Capacity-count_)return false;
        for(uint32_t i=0;i<count;++i) {
            if(!valid(requested[i]))return false;
            for(uint32_t j=0;j<count_;++j)if(overlaps(requested[i],leases_[j]))return false;
            for(uint32_t j=0;j<i;++j)if(overlaps(requested[i],requested[j]))return false;
        }
        for(uint32_t i=0;i<count;++i)leases_[count_++]=requested[i];
        return true;
    }
    bool AllowOrdinary(uintptr_t parent,uint64_t start,uint64_t length) const {
        if(!parent || start>~uint64_t{0}-length)return false;
        if(!length)return true;
        const BlockWriteLease request{parent,0,start,length};
        for(uint32_t i=0;i<count_;++i)if(overlaps(request,leases_[i]))return false;
        return true;
    }
    bool AllowOwner(uintptr_t parent,uintptr_t owner,uint64_t start,uint64_t length) const {
        if(!parent || !owner || !length || start>~uint64_t{0}-length)return false;
        for(uint32_t i=0;i<count_;++i) {
            const auto& x=leases_[i];
            if(x.parent==parent && x.owner==owner && start>=x.start &&
               start+length<=x.start+x.length)return true;
        }
        return false;
    }
    bool References(uintptr_t identity) const {
        if(!identity)return false;
        for(uint32_t i=0;i<count_;++i)
            if(leases_[i].parent==identity || leases_[i].owner==identity)return true;
        return false;
    }
    uint32_t Count() const {return count_;}
};
}
