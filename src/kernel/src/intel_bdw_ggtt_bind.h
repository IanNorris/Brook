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
    // A Gen8 render context consumes 20 pages; retain it with ring and
    // status pages in the same all-or-nothing ownership transaction.
    static constexpr uint32_t MaximumPages=32;
    uint32_t index[MaximumPages]{};
    uint64_t dma[MaximumPages]{},expected[MaximumPages]{};
    uint32_t count=2;
};
enum class BdwGgttBindState : uint8_t { Fresh, Working, Bound, Restored, Quarantined };

class BdwGgttBind {
    BdwGgttBindState state_=BdwGgttBindState::Fresh;
    BdwGgttLease lease_{};
    uint64_t restore_[BdwGgttLease::MaximumPages]{};
    const void* owner_=nullptr;
    unsigned touched_=0;bool takeover_=false;
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
        for(unsigned i=0;i<touched_;++i)if(!io.write(lease_.index[i],restore_[i]))ok=false;
        if(!io.invalidate())ok=false;
        for(unsigned i=0;i<touched_;++i){uint64_t pte=0;if(!io.read(lease_.index[i],pte) || pte!=restore_[i])ok=false;}
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
    // A new transaction may reuse this software object only after the owner
    // completed the previous PTE restoration and invalidation.
    bool Recycle(){
        if(!SafeToRelease())return false;
        state_=BdwGgttBindState::Fresh;lease_={};owner_=nullptr;touched_=0;takeover_=false;
        for(auto& value:restore_)value=0;
        return true;
    }
private:
    template<class Io> bool BindImpl(const BdwGgttLease& l,Io& io,uint64_t scratchPte,bool takeover) {
        if(state_!=BdwGgttBindState::Fresh || !Geometry(l))return false;
        state_=BdwGgttBindState::Working;
        if(!io.validateLease(l) || !io.quiescent()){state_=BdwGgttBindState::Fresh;return false;}
        uint64_t before[BdwGgttLease::MaximumPages]{};
        for(unsigned i=0;i<l.count;++i)if(!io.read(l.index[i],before[i]) || before[i]!=l.expected[i]){state_=BdwGgttBindState::Fresh;return false;}
        lease_=l;owner_=static_cast<const void*>(&io);takeover_=takeover;
        for(unsigned i=0;i<l.count;++i)restore_[i]=takeover?scratchPte:l.expected[i];
        for(unsigned i=0;i<l.count;++i){touched_=i+1;if(!io.write(l.index[i],l.dma[i]|1ull)){Restore(io);return false;}}
        if(!io.invalidate()){Restore(io);return false;}
        for(unsigned i=0;i<l.count;++i){uint64_t actual=0;if(!io.read(l.index[i],actual) || actual!=(l.dma[i]|1ull)){Restore(io);return false;}}
        state_=BdwGgttBindState::Bound;return true;
    }
public:
    template<class Io> bool Bind(const BdwGgttLease& l,Io& io) {return BindImpl(l,io,0,false);}
    // Only after a real device takeover: the backend must establish exclusive
    // ownership of this exact range, exclude every live firmware/display user,
    // and keep scratchDma pinned and GPU-accessible as long as these PTEs can
    // survive. Unknown old PTEs are compared, but are never replayed. On any
    // ambiguous write or invalidation all potentially referenced pages remain
    // quarantined. There is no production takeover backend yet.
    template<class Io> bool BindTakenOver(const BdwGgttLease& l,Io& io,uint64_t scratchDma) {
        constexpr uint64_t limit=uint64_t{1}<<39,rmrrStart=0xdd000000ull,rmrrEnd=0xdf800000ull;
        if(state_!=BdwGgttBindState::Fresh || !Geometry(l) || !scratchDma || (scratchDma&4095) ||
           scratchDma>=limit || (scratchDma<rmrrEnd && scratchDma+4096>rmrrStart))return false;
        for(unsigned i=0;i<l.count;++i)if(scratchDma==l.dma[i])return false;
        state_=BdwGgttBindState::Working;
        const bool owned=io.validateTakeover(l,scratchDma);
        state_=BdwGgttBindState::Fresh;
        return owned && BindImpl(l,io,scratchDma|1ull,true);
    }
    template<class Io> bool Retire(Io& io) {
        if(state_!=BdwGgttBindState::Bound)return false;
        state_=BdwGgttBindState::Working;
        if(owner_!=static_cast<const void*>(&io) || !io.validateLease(lease_)){
            state_=BdwGgttBindState::Quarantined;return false;
        }
        // GPU completion or reset must be proved by the backend, not inferred
        // from a timeout or ring head movement.
        if(!io.quiescent()){state_=BdwGgttBindState::Quarantined;return false;}
        return Restore(io);
    }
    // After a proven engine stop, invalidate every exact PTE installed by a
    // takeover instead of leaving mappings to a boot-lifetime scratch page.
    // The owner must exclude firmware/display use of this reserved range and
    // retain the lease across compare, clear, invalidation and readback.
    // Any mismatch or ambiguous operation quarantines the mapping/pages.
    template<class Io> bool RetireToInvalid(Io& io) {
        if(state_!=BdwGgttBindState::Bound || !takeover_)return false;
        state_=BdwGgttBindState::Working;
        if(owner_!=static_cast<const void*>(&io) || !io.validateLease(lease_) || !io.quiescent()){
            state_=BdwGgttBindState::Quarantined;return false;
        }
        for(unsigned i=0;i<lease_.count;++i){uint64_t value=0;
            if(!io.read(lease_.index[i],value) || value!=(lease_.dma[i]|1ull)){
                state_=BdwGgttBindState::Quarantined;return false;
            }
        }
        bool ok=true;
        for(unsigned i=0;i<lease_.count;++i)if(!io.write(lease_.index[i],0))ok=false;
        if(!io.invalidate())ok=false;
        for(unsigned i=0;i<lease_.count;++i){uint64_t value=~uint64_t{0};
            if(!io.read(lease_.index[i],value) || value)ok=false;
        }
        state_=ok?BdwGgttBindState::Restored:BdwGgttBindState::Quarantined;
        return ok;
    }
};
}
