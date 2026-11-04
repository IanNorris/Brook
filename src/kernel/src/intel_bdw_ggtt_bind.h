#pragma once
#include <stdint.h>
namespace brook {
// Caller/backend owns the lease and pins throughout this transaction. A lease
// is authoritative only if the backend validates it under exclusive graphics
// ownership. Numeric GGTT addresses or old PTE contents do not confer ownership.
// Calls must be serialized. validateLease must retain exclusive ownership and page pins through retire,
// and verify DMA visibility, non-aliasing backing and a valid reservation.
// invalidate must order PTE stores and provide the platform's required GGTT
// invalidation guarantee, including draining earlier failed/posted writes; a
// readback alone is insufficient. No production
// backend exists yet, so this transaction does not touch hardware.
struct BdwGgttLease {
    uint64_t token=0;
    // Context (two pages), engine status, a multi-page ring, batch, source,
    // destination and completion can be retained in one transaction.
    static constexpr uint32_t MaximumPages=16;
    uint32_t index[MaximumPages]{};
    uint64_t dma[MaximumPages]{},expected[MaximumPages]{};
    uint32_t count=2;
};
enum class BdwGgttBindState : uint8_t { Fresh, Bound, Restored, Quarantined };

class BdwGgttBind {
    BdwGgttBindState state_=BdwGgttBindState::Fresh;
    BdwGgttLease lease_{};
    const void* owner_=nullptr;
    unsigned touched_=0;
    static bool Geometry(const BdwGgttLease& l) {
        constexpr uint64_t limit=uint64_t{1}<<39,rmrrStart=0xdd000000ull,rmrrEnd=0xdf800000ull;
        if(!l.token || !l.count || l.count>BdwGgttLease::MaximumPages)return false;
        for(unsigned i=0;i<l.count;++i) {
            for(unsigned j=0;j<i;++j)if(l.index[i]==l.index[j] || l.dma[i]==l.dma[j])return false;
            if(l.index[i]<65536 || l.index[i]>=1048576 || !l.dma[i] || (l.dma[i]&4095) || l.dma[i]>=limit ||
               (l.dma[i]<rmrrEnd && l.dma[i]+4096>rmrrStart))return false;
        }
        return true;
    }
    template<class Io> bool Restore(Io& io) {
        if(owner_!=static_cast<const void*>(&io) || !io.validateLease(lease_)){
            state_=BdwGgttBindState::Quarantined;return false;
        }
        // A failed write may have reached hardware: restore that slot too.
        bool ok=true;
        for(unsigned i=0;i<touched_;++i)if(!io.write(lease_.index[i],lease_.expected[i]))ok=false;
        if(!io.invalidate())ok=false;
        for(unsigned i=0;i<touched_;++i){uint64_t pte=0;if(!io.read(lease_.index[i],pte) || pte!=lease_.expected[i])ok=false;}
        state_=ok?BdwGgttBindState::Restored:BdwGgttBindState::Quarantined;
        return ok;
    }
public:
    BdwGgttBind()=default;
    BdwGgttBind(const BdwGgttBind&)=delete;
    BdwGgttBind& operator=(const BdwGgttBind&)=delete;
    BdwGgttBind(BdwGgttBind&&)=delete;
    BdwGgttBind& operator=(BdwGgttBind&&)=delete;
    BdwGgttBindState State() const {return state_;}
    // Fresh has not touched hardware; Restored has retired all mutations.
    bool SafeToRelease() const {return (state_==BdwGgttBindState::Fresh && !touched_) || state_==BdwGgttBindState::Restored;}
    template<class Io> bool Bind(const BdwGgttLease& l,Io& io) {
        if(state_!=BdwGgttBindState::Fresh || !Geometry(l) || !io.validateLease(l) || !io.quiescent())return false;
        uint64_t before[BdwGgttLease::MaximumPages]{};
        for(unsigned i=0;i<l.count;++i)if(!io.read(l.index[i],before[i]) || before[i]!=l.expected[i])return false;
        lease_=l;owner_=static_cast<const void*>(&io);
        for(unsigned i=0;i<l.count;++i){touched_=i+1;if(!io.write(l.index[i],l.dma[i]|1ull)){Restore(io);return false;}}
        if(!io.invalidate()){Restore(io);return false;}
        for(unsigned i=0;i<l.count;++i){uint64_t actual=0;if(!io.read(l.index[i],actual) || actual!=(l.dma[i]|1ull)){Restore(io);return false;}}
        state_=BdwGgttBindState::Bound;return true;
    }
    template<class Io> bool Retire(Io& io) {
        if(state_!=BdwGgttBindState::Bound)return false;
        if(owner_!=static_cast<const void*>(&io) || !io.validateLease(lease_)){
            state_=BdwGgttBindState::Quarantined;return false;
        }
        // GPU completion or reset must be proved by the backend, not inferred
        // from a timeout or ring head movement.
        if(!io.quiescent()){state_=BdwGgttBindState::Quarantined;return false;}
        return Restore(io);
    }
};
}
