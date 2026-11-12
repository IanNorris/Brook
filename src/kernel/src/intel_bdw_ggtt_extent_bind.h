#pragma once
#include <stdint.h>
#include "intel_bdw_ggtt_space.h"

namespace brook {
// A caller-owned, immutable array of pinned DMA pages and its exact software
// reservation. Retain both through SafeToRelease(), including quarantine.
// validateLease must independently prove exclusive device ownership, the
// reservation, non-aliasing pages, DMA/cache safety and firmware exclusion.
struct BdwGgttExtentLease { BdwGgttReservation reservation{};const uint64_t* dma=nullptr; };
enum class BdwGgttExtentState : uint8_t { Fresh, Working, Bound, Restored, Quarantined };
class BdwGgttExtentBind {
    BdwGgttExtentState state_=BdwGgttExtentState::Fresh;
    BdwGgttExtentLease lease_{};const void* owner_=nullptr;uint32_t touched_=0;
    const uint64_t* expected_=nullptr;uint64_t scratchDma_=0;
    static bool Geometry(const BdwGgttExtentLease& l) {
        const auto& r=l.reservation;
        if(!r.token || !r.count || r.count>65536 || r.first<65536 || r.first>=1048576 ||
           r.count>1048576-r.first || !l.dma)return false;
        for(uint32_t i=0;i<r.count;++i){const uint64_t p=l.dma[i];
            if(!p || (p&4095) || p>=(uint64_t{1}<<39) || (p>=0xdd000000ull && p<0xdf800000ull))return false;
        }
        return true;
    }
    template<class Io> bool Restore(Io& io,bool requireInstalled,bool toInvalid=false) {
        if(owner_!=static_cast<const void*>(&io) || !io.validateLease(lease_) ||
           (scratchDma_ && !io.validateTakeover(lease_,scratchDma_)) || !io.quiescent() || !io.invalidate()){
            state_=BdwGgttExtentState::Quarantined;return false;
        }
        // A failed write may have landed. After draining, only our exact PTE
        // or the original invalid entry may be cleared; never replay unknowns.
        for(uint32_t i=0;i<touched_;++i){uint64_t value=0;
            if(!io.read(lease_.reservation.first+i,value) ||
               (value!=(lease_.dma[i]|1ull) && (requireInstalled || value!=(expected_?expected_[i]:0)))){
                state_=BdwGgttExtentState::Quarantined;return false;
            }
        }
        const uint64_t restore=toInvalid?0:(scratchDma_?(scratchDma_|1ull):0);
        bool ok=true;for(uint32_t i=0;i<touched_;++i)if(!io.write(lease_.reservation.first+i,restore))ok=false;
        if(!io.invalidate())ok=false;
        for(uint32_t i=0;i<touched_;++i){uint64_t value=~uint64_t{0};
            if(!io.read(lease_.reservation.first+i,value) || value!=restore)ok=false;
        }
        state_=ok?BdwGgttExtentState::Restored:BdwGgttExtentState::Quarantined;return ok;
    }
public:
    BdwGgttExtentBind()=default;
    BdwGgttExtentBind(const BdwGgttExtentBind&)=delete;
    BdwGgttExtentBind& operator=(const BdwGgttExtentBind&)=delete;
    BdwGgttExtentState State()const{return state_;}
    bool SafeToRelease()const{return (state_==BdwGgttExtentState::Fresh && !touched_) || state_==BdwGgttExtentState::Restored;}
    bool Recycle(){
        if(!SafeToRelease())return false;
        state_=BdwGgttExtentState::Fresh;lease_={};owner_=nullptr;touched_=0;expected_=nullptr;scratchDma_=0;return true;
    }
private:
    template<class Io> bool BindImpl(const BdwGgttExtentLease& l,Io& io,const uint64_t* expected,uint64_t scratch){
        if(state_!=BdwGgttExtentState::Fresh || !Geometry(l))return false;
        state_=BdwGgttExtentState::Working;
        if(!io.validateLease(l) || (scratch && !io.validateTakeover(l,scratch)) || !io.quiescent()){state_=BdwGgttExtentState::Fresh;return false;}
        for(uint32_t i=0;i<l.reservation.count;++i){uint64_t value=~uint64_t{0};
            if(!io.read(l.reservation.first+i,value) || value!=(expected?expected[i]:0)){state_=BdwGgttExtentState::Fresh;return false;}
        }
        lease_=l;owner_=static_cast<const void*>(&io);expected_=expected;scratchDma_=scratch;
        for(uint32_t i=0;i<l.reservation.count;++i){touched_=i+1;
            if(!io.write(l.reservation.first+i,l.dma[i]|1ull)){(void)Restore(io,false);return false;}
        }
        if(!io.invalidate()){state_=BdwGgttExtentState::Quarantined;return false;}
        for(uint32_t i=0;i<l.reservation.count;++i){uint64_t value=0;
            if(!io.read(l.reservation.first+i,value) || value!=(l.dma[i]|1ull)){state_=BdwGgttExtentState::Quarantined;return false;}
        }
        state_=BdwGgttExtentState::Bound;return true;
    }
public:
    template<class Io> bool Bind(const BdwGgttExtentLease& l,Io& io){return BindImpl(l,io,nullptr,0);}
    // Explicit takeover only: expected is an immutable, pinned snapshot of
    // the entire range. The owner must exclude all old users and retain the
    // scratch page across warm boots until its mappings are retired.
    template<class Io> bool BindTakenOver(const BdwGgttExtentLease& l,Io& io,const uint64_t* expected,uint64_t scratch){
        if(!expected || !Geometry(l) || !scratch || (scratch&4095) || scratch>=(uint64_t{1}<<39) ||
           (scratch>=0xdd000000ull && scratch<0xdf800000ull))return false;
        for(uint32_t i=0;i<l.reservation.count;++i)if(l.dma[i]==scratch)return false;
        return BindImpl(l,io,expected,scratch);
    }
    template<class Io> bool Retire(Io& io){
        if(state_!=BdwGgttExtentState::Bound)return false;
        state_=BdwGgttExtentState::Working;return Restore(io,true);
    }
    // A fully bound takeover can instead invalidate every installed entry.
    // This removes scratch references after a clean retirement; it cannot
    // handle a reset or panic which occurs before the transaction completes.
    template<class Io> bool RetireToInvalid(Io& io){
        if(state_!=BdwGgttExtentState::Bound || !scratchDma_)return false;
        state_=BdwGgttExtentState::Working;return Restore(io,true,true);
    }
};
}
