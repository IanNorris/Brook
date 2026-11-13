#pragma once
#include "intel_bdw_scanout_lifetime.h"
#include "intel_bdw_ggtt_extent_bind.h"

namespace brook {
// Unwired adapter. The caller serializes the scanout, handoff and GGTT spaces,
// retains this object on Retained, and provides a backend proving exclusive
// hardware ownership, DMA/cache safety and display/engine quiescence.
// validateDisplayGuard must prove the reserved tail PTEs are prepared for the
// display workaround and remain so until the mapping and plane are retired;
// reserving their indices does not initialize them or pin a scratch page.
// No destructor implicitly releases a possibly live mapping or pinned buffer.
template<class Io> class BdwScanoutGgttMapping {
    // SNB through BDW primary-plane VT-d workaround: 128 guard PTEs,
    // and a 256 KiB aligned surface. Apply regardless of current VT-d mode.
    static constexpr uint32_t GuardPages=128,SurfaceAlignment=64;
    BdwGgttSpace& space_;Io& io_;BdwGpuBuffer* buffer_=nullptr;
    uint64_t token_=0,identity_=0;BdwGgttReservation reservation_{};
    const uint64_t* pages_=nullptr;uint32_t count_=0;bool takeover_=false;BdwGgttExtentBind bind_{};
    struct CheckedIo {
        BdwScanoutGgttMapping& self;
        bool validateLease(const BdwGgttExtentLease& l){
            uint32_t count=0;const auto* pages=BdwGpuBufferPages(self.buffer_,count);
            return self.buffer_ && self.identity_==BdwGpuBufferIdentity(self.buffer_) &&
                self.space_.owns(self.reservation_) && l.reservation.first==self.reservation_.first &&
                self.reservation_.count==self.count_+GuardPages && l.reservation.count==self.count_ &&
                l.reservation.token==self.reservation_.token && !(l.reservation.first&(SurfaceAlignment-1)) &&
                l.dma==self.pages_ && pages==self.pages_ && count==self.count_ && count==l.reservation.count &&
                BdwGpuBufferScanoutMappingMatches(self.buffer_,self.token_) &&
                self.io_.validateDisplayGuard(self.reservation_,l) && self.io_.validateLease(l);
        }
        bool quiescent(){return self.io_.quiescent();}
        bool validateTakeover(const BdwGgttExtentLease& l,uint64_t scratch){
            return validateLease(l) && self.io_.validateTakeover(l,scratch);
        }
        bool read(uint32_t i,uint64_t& v){return self.io_.read(i,v);}
        bool write(uint32_t i,uint64_t v){return self.io_.write(i,v);}
        bool invalidate(){return self.io_.invalidate();}
    } checked_{*this};
    bool Cleanup(){
        if(!buffer_ || !bind_.SafeToRelease())return false;
        if(reservation_.token){if(!space_.release(reservation_))return false;reservation_={};}
        if(!BdwGpuBufferEndScanoutMapping(buffer_,token_))return false;
        if(!bind_.Recycle())return false;
        BdwGpuBufferUnref(buffer_);buffer_=nullptr;token_=identity_=0;pages_=nullptr;count_=0;takeover_=false;return true;
    }
    template<bool Takeover> BdwScanoutResult BindImpl(BdwScanoutResources& scan,uint32_t alignment){
        if(buffer_ || !scan.policy.pending)return BdwScanoutResult::Rejected;
        BdwScanoutResources::Entry* selected=nullptr;
        for(auto& e:scan.entries)if(e.buffer && BdwGpuBufferIdentity(e.buffer)==scan.policy.pending)selected=&e;
        if(!selected || !BdwGpuBufferBeginScanoutMapping(selected->buffer,selected->token))return BdwScanoutResult::Rejected;
        buffer_=selected->buffer;token_=selected->token;identity_=BdwGpuBufferIdentity(buffer_);BdwGpuBufferRef(buffer_);
        pages_=BdwGpuBufferPages(buffer_,count_);
        if(!pages_ || !count_ || count_>65536-GuardPages || !alignment || (alignment&(alignment-1)) ||
           !space_.reserveExtent(count_+GuardPages,alignment<SurfaceAlignment?SurfaceAlignment:alignment,reservation_))
            return Cleanup()?BdwScanoutResult::Rejected:BdwScanoutResult::Retained;
        const BdwGgttExtentLease lease{{reservation_.first,count_,reservation_.token},pages_};bool bound=false;
        if constexpr(Takeover){
            // The backend retains the immutable snapshot and scratch for the
            // transaction, and proves ownership; reservation alone is not proof.
            const uint64_t* expected=nullptr;uint64_t scratch=0;
            if(io_.prepareTakeover(lease,expected,scratch)){
                takeover_=true;bound=bind_.BindTakenOver(lease,checked_,expected,scratch);
            }
        }else bound=bind_.Bind(lease,checked_);
        if(bound)return BdwScanoutResult::Completed;
        return Cleanup()?BdwScanoutResult::Rejected:BdwScanoutResult::Retained;
    }
public:
    BdwScanoutGgttMapping(BdwGgttSpace& space,Io& io):space_(space),io_(io){}
    BdwScanoutGgttMapping(const BdwScanoutGgttMapping&)=delete;
    BdwScanoutGgttMapping& operator=(const BdwScanoutGgttMapping&)=delete;
    bool Held()const{return buffer_!=nullptr;}
    BdwGgttReservation Reservation()const{return reservation_;}
    BdwScanoutResult Bind(BdwScanoutResources& scan,uint32_t alignment){
        return BindImpl<false>(scan,alignment);
    }
    BdwScanoutResult BindTakenOver(BdwScanoutResources& scan,uint32_t alignment){return BindImpl<true>(scan,alignment);}
    // Scanout must already have stopped referring to this buffer. The backend
    // additionally proves display and engine quiescence before PTE removal.
    BdwScanoutResult Retire(const BdwScanoutResources& scan){
        if(!buffer_ || scan.policy.active==identity_ || scan.policy.pending==identity_)return BdwScanoutResult::Rejected;
        if(bind_.State()==BdwGgttExtentState::Bound &&
           !(takeover_?bind_.RetireToInvalid(checked_):bind_.Retire(checked_)))return BdwScanoutResult::Retained;
        return Cleanup()?BdwScanoutResult::Completed:BdwScanoutResult::Retained;
    }
};
}
