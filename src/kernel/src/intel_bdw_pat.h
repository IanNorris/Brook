#pragma once
#include <stdint.h>
namespace brook {
// This predicate compares an already trusted, current private-PAT snapshot
// with the policy expected by Brook's legacy 48-bit BCS page tables. It does
// not read hardware, claim GT ownership, establish cache coherency or pin the
// policy against changes. The owner must hold those guarantees through use.
inline bool BdwLegacyBcsPatCompatible(uint64_t descriptor,uint32_t low,uint32_t high,bool uncachedDestination=false) {
    constexpr uint64_t legacy48Ppgtt=0x119; // valid, legacy, 48-bit, PPGTT
    constexpr uint32_t tableWbLlc=0x07,leafWbLlcEllc=0x0b;
    if((descriptor&legacy48Ppgtt)!=legacy48Ppgtt)return false;
    // PPGTT directory entries use index 0; cached leaf PTEs use index 4.
    return (low&0xffu)==tableWbLlc && (high&0xffu)==leafWbLlcEllc &&
        (!uncachedDestination || (low>>24)==0); // private PAT slot 3
}

enum class BdwPatLeaseState : uint8_t { Fresh, Working, Active, Restored, Quarantined };
struct BdwPatTestResult {
    bool attempted=false,resetComplete=false,established=false,restored=false;
    uint32_t beforeLo=0,beforeHi=0,activeLo=0,activeHi=0,afterLo=0,afterHi=0;
};
// Transaction for the two PAT entries selected by the legacy BCS page
// tables. The backend must serialize all GPU and firmware users, prove that
// both the transition and final policy are safe for active display consumers,
// hold forcewake and audited UC mappings, and retain that ownership while the
// lease is active. drainAndInvalidate() must drain *all* preceding writes,
// including ones that returned failure, and invalidate every affected GPU
// translation/cache before it reports success. No MMIO owner is implied.
class BdwPatLease {
    BdwPatLeaseState state_=BdwPatLeaseState::Fresh;
    const void* owner_=nullptr;
    uint32_t oldLo_=0,oldHi_=0;
    template<class Io> bool stable(Io& io,uint32_t& lo,uint32_t& hi) {
        uint32_t lo2=0,hi2=0;
        return io.owned() && io.readPrivatePat(lo,hi) && io.readPrivatePat(lo2,hi2) &&
            lo!=0xffffffffu && hi!=0xffffffffu && lo==lo2 && hi==hi2;
    }
    template<class Io> bool restore(Io& io) {
        if(owner_!=static_cast<const void*>(&io) || !io.owned())return false;
        // Attempt both stores even if the first one fails: either may have
        // reached the device. Completion requires the stronger drain contract.
        const bool lo=io.writePatLow(oldLo_),hi=io.writePatHigh(oldHi_);
        const bool drained=io.drainAndInvalidate();uint32_t a=0,b=0;
        return lo && hi && drained && stable(io,a,b) && a==oldLo_ && b==oldHi_;
    }
public:
    BdwPatLease()=default;
    BdwPatLease(const BdwPatLease&)=delete;
    BdwPatLease& operator=(const BdwPatLease&)=delete;
    BdwPatLeaseState State()const{return state_;}
    template<class Io> bool Establish(Io& io,uint64_t descriptor,bool uncachedDestination=false) {
        if(state_!=BdwPatLeaseState::Fresh)return false;
        state_=BdwPatLeaseState::Working;owner_=&io;
        uint32_t lo=0,hi=0;
        if(!io.owned() || !io.allEnginesQuiescent() || !stable(io,lo,hi)) {
            state_=BdwPatLeaseState::Fresh;owner_=nullptr;return false;
        }
        const uint32_t targetLo=(lo&(uncachedDestination?0x00ffff00u:0xffffff00u))|0x07u;
        const uint32_t targetHi=(hi&~0xffu)|0x0bu;
        if(!BdwLegacyBcsPatCompatible(descriptor,targetLo,targetHi,uncachedDestination) ||
           !io.safePatTransition(lo,hi,targetLo,targetHi)) {
            state_=BdwPatLeaseState::Fresh;owner_=nullptr;return false;
        }
        oldLo_=lo;oldHi_=hi;
        if(lo==targetLo && hi==targetHi){state_=BdwPatLeaseState::Active;return true;}
        uint32_t checkLo=0,checkHi=0;
        if(!stable(io,checkLo,checkHi) || checkLo!=lo || checkHi!=hi) {
            state_=BdwPatLeaseState::Fresh;owner_=nullptr;return false;
        }
        if(!io.writePatLow(targetLo) || !io.writePatHigh(targetHi) ||
           !io.drainAndInvalidate() || !stable(io,checkLo,checkHi) ||
           checkLo!=targetLo || checkHi!=targetHi) {
            state_=restore(io)?BdwPatLeaseState::Restored:BdwPatLeaseState::Quarantined;
            return false;
        }
        state_=BdwPatLeaseState::Active;return true;
    }
    // Return to the inherited policy only after the owner has retired or
    // reset its GPU work. Failed restoration is uncertain and quarantines
    // the lease; the caller must retain all potentially referenced memory.
    template<class Io> bool Release(Io& io) {
        if(state_!=BdwPatLeaseState::Active || owner_!=static_cast<const void*>(&io))return false;
        state_=BdwPatLeaseState::Working;
        if(!io.owned() || !io.allEnginesQuiescent() || !restore(io)) {
            state_=BdwPatLeaseState::Quarantined;return false;
        }
        state_=BdwPatLeaseState::Restored;return true;
    }
};

// BAR0+0x4000 mapped UC. The owner must validate the mapping, hold GT
// forcewake/exclusion and implement the drain and GPU invalidation contract.
// Only the approved old/new low and high halves can be written.
template<class Owner> class BdwPatMmio {
    Owner& owner_;volatile uint32_t* page_;bool authorized_=false;
    bool uncachedDestination_;
    uint32_t oldLo_=0,oldHi_=0,newLo_=0,newHi_=0;
    bool mapped()const{return page_ && !(reinterpret_cast<uintptr_t>(page_)&3);}
public:
    BdwPatMmio(Owner& owner,volatile uint32_t* page,bool uncachedDestination=false)
        :owner_(owner),page_(page),uncachedDestination_(uncachedDestination){}
    BdwPatMmio(const BdwPatMmio&)=delete;
    BdwPatMmio& operator=(const BdwPatMmio&)=delete;
    bool owned(){return mapped() && owner_.owned();}
    bool allEnginesQuiescent(){return owned() && owner_.allEnginesQuiescent();}
    bool readPrivatePat(uint32_t& lo,uint32_t& hi){
        if(!owned())return false;
        __asm__ volatile("lfence":::"memory");lo=page_[0xe0/4];hi=page_[0xe4/4];
        __asm__ volatile("lfence":::"memory");return owner_.owned();
    }
    bool safePatTransition(uint32_t lo,uint32_t hi,uint32_t nextLo,uint32_t nextHi){
        const uint32_t lowChanges=lo^nextLo;
        const bool changedScanoutSlot=(lowChanges&0xff000000u)!=0;
        if(authorized_ || !owned() || (lowChanges&0x00ffff00u) || ((hi^nextHi)&~0xffu) ||
           (changedScanoutSlot && (!uncachedDestination_ || (lo>>24)!=3 || (nextLo>>24)!=0)) ||
           (nextLo&0xffu)!=7 || (nextHi&0xffu)!=11 ||
           !owner_.safePatTransition(lo,hi,nextLo,nextHi))return false;
        oldLo_=lo;oldHi_=hi;newLo_=nextLo;newHi_=nextHi;authorized_=true;return true;
    }
    bool writePatLow(uint32_t v){
        if(!authorized_ || !owned() || (v!=oldLo_ && v!=newLo_))return false;
        page_[0xe0/4]=v;__asm__ volatile("mfence":::"memory");return owner_.owned();
    }
    bool writePatHigh(uint32_t v){
        if(!authorized_ || !owned() || (v!=oldHi_ && v!=newHi_))return false;
        page_[0xe4/4]=v;__asm__ volatile("mfence":::"memory");return owner_.owned();
    }
    bool drainAndInvalidate(){return owned() && owner_.drainAndInvalidate();}
};
}
