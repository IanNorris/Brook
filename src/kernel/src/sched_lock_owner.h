#pragma once
#include <stdint.h>

namespace brook {
enum class OwnerWaitObservation : unsigned {
    MissingOwner, TicketMismatch, Changed, Self, MissingSteal, Preempted, NotPreempted, AlreadyGranted, Count
};
constexpr unsigned OwnerWaitKinds=unsigned(OwnerWaitObservation::Count);
struct ReadyLockOwnerCpu {
    uint64_t published=0,cleared=0,waits=0,observations=0,otherHints=0,errors=0;
    uint64_t kind[OwnerWaitKinds]{};
    uint64_t hintEligible=0,hintAttempted=0,hintUnsupported=0,hintErrors=0,hintBudgetExhausted=0,hintRaces=0,ifErrors=0;
};
struct ReadyLockOwnerSnapshot {
    bool enabled=false,saturated=false,yieldEnabled=false;
    unsigned cpus=0;
    uint64_t owner=0;
    ReadyLockOwnerCpu cpu[64]{};
};
// Advisory publication only. The ticket lock remains the sole ownership and
// ordering authority. Publishers hold that lock, with IF clear; each CPU has
// one counter writer. All concurrent reader accesses are atomic.
class ReadyLockOwnerObservation {
    uint32_t control_=0;
    alignas(64) uint64_t owner_=0;
    struct alignas(64) Cpu {ReadyLockOwnerCpu value{};} cpu_[64]{};
    static void Increment(uint64_t& value){
        const auto old=__atomic_load_n(&value,__ATOMIC_RELAXED);
        if(old!=~uint64_t(0))__atomic_store_n(&value,old+1,__ATOMIC_RELAXED);
    }
public:
    static constexpr uint64_t Valid=uint64_t(1)<<63;
    static uint64_t Pack(unsigned cpu,uint32_t ticket){return cpu<64?Valid|(uint64_t(cpu)<<32)|ticket:0;}
    static unsigned CpuIndex(uint64_t value){return unsigned((value>>32)&63);}
    static uint32_t Ticket(uint64_t value){return uint32_t(value);}
    bool Enabled()const{return (__atomic_load_n(&control_,__ATOMIC_ACQUIRE)&1)!=0;}
    bool YieldEnabled()const{return (__atomic_load_n(&control_,__ATOMIC_ACQUIRE)&2)!=0;}
    void Mode(bool enabled){if(enabled)__atomic_fetch_or(&control_,1u,__ATOMIC_RELEASE);else __atomic_store_n(&control_,0u,__ATOMIC_RELEASE);}
    bool YieldMode(bool enabled){
        auto old=__atomic_load_n(&control_,__ATOMIC_ACQUIRE);if(enabled && !(old&1))return false;
        return __atomic_compare_exchange_n(&control_,&old,enabled?old|2u:old&~2u,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
    }
    uint64_t Owner()const{return __atomic_load_n(&owner_,__ATOMIC_ACQUIRE);}
    // Call only AFTER actual ticket ownership, also for successful try claims.
    void Acquired(unsigned cpu,uint32_t ticket){
        if(cpu>=64 || !Enabled())return;
        auto& c=cpu_[cpu].value;
        if(Owner())Increment(c.errors);
        __atomic_store_n(&owner_,Pack(cpu,ticket),__ATOMIC_RELEASE);Increment(c.published);
    }
    // Call BEFORE serving advances, even when admission was since disabled.
    // Otherwise an old release could erase its successor's publication.
    void Releasing(unsigned cpu,uint32_t serving){
        const auto owner=Owner();if(!owner || cpu>=64)return;
        auto& c=cpu_[cpu].value;
        if(owner!=Pack(cpu,serving))Increment(c.errors);
        __atomic_store_n(&owner_,uint64_t(0),__ATOMIC_RELEASE);Increment(c.cleared);
    }
    void Waiting(unsigned cpu){if(cpu<64 && Enabled())Increment(cpu_[cpu].value.waits);}
    // Compatibility observation-only entry for helpers without a wait ticket.
    template<class Serving,class Hint> void Observe(unsigned cpu,Serving serving,Hint hint){
        bool spent=false;
        ObserveImpl(cpu,serving,hint,false,0,spent,[](unsigned,int64_t*){return false;},[]{return uint64_t(0);});
    }
    template<class Serving,class Hint,class Yield,class Flags>
    void ObserveWait(unsigned cpu,uint32_t ticket,bool& spent,Serving serving,Hint hint,Yield yield,Flags flags){
        ObserveImpl(cpu,serving,hint,true,ticket,spent,yield,flags);
    }
private:
    template<class Serving,class Hint,class Yield,class Flags>
    void ObserveImpl(unsigned cpu,Serving serving,Hint hint,bool hasTicket,uint32_t ticket,bool& spent,Yield yield,Flags flags){
        if(cpu>=64 || !Enabled())return;
        auto& c=cpu_[cpu].value;Increment(c.observations);
        const uint32_t before=serving();const auto owner=Owner();
        OwnerWaitObservation result;
        if(hasTicket && before==ticket)result=OwnerWaitObservation::AlreadyGranted;
        else if(!owner)result=OwnerWaitObservation::MissingOwner;
        else if(Ticket(owner)!=before)result=OwnerWaitObservation::TicketMismatch;
        else if(CpuIndex(owner)==cpu)result=OwnerWaitObservation::Self;
        else {
            uint8_t value=0;const bool available=hint(CpuIndex(owner),&value);
            if(before!=serving() || owner!=Owner())result=OwnerWaitObservation::Changed;
            else if(!available)result=OwnerWaitObservation::MissingSteal;
            else {
                if(value&~1u)Increment(c.otherHints);
                result=(value&1u)?OwnerWaitObservation::Preempted:OwnerWaitObservation::NotPreempted;
            }
        }
        Increment(c.kind[unsigned(result)]);
        if(result!=OwnerWaitObservation::Preempted || !hasTicket || !YieldEnabled())return;
        Increment(c.hintEligible);
        if(spent){Increment(c.hintBudgetExhausted);return;}
        const uint64_t beforeFlags=flags();
        if(beforeFlags&0x200){Increment(c.ifErrors);return;}
        // Advisory only: a race after this recheck may change the target. It
        // cannot grant admission. One spent budget covers unsupported/errors too.
        if(before!=serving() || owner!=Owner()){Increment(c.hintRaces);return;}
        if(!YieldEnabled())return;
        spent=true;int64_t returned=0;
        if(!yield(CpuIndex(owner),&returned))Increment(c.hintUnsupported);
        else {Increment(c.hintAttempted);if(returned!=0)Increment(c.hintErrors);}
        if((flags()^beforeFlags)&0x200)Increment(c.ifErrors);
    }
public:
    void Read(unsigned cpus,ReadyLockOwnerSnapshot* out)const{
        if(!out)return;*out={};out->enabled=Enabled();out->yieldEnabled=YieldEnabled();out->owner=Owner();out->cpus=cpus<64?cpus:64;
        for(unsigned i=0;i<out->cpus;++i){const auto& c=cpu_[i].value;auto& o=out->cpu[i];
#define OWNER_READ(f) o.f=__atomic_load_n(&c.f,__ATOMIC_RELAXED);out->saturated|=o.f==~uint64_t(0)
            OWNER_READ(published);OWNER_READ(cleared);OWNER_READ(waits);OWNER_READ(observations);OWNER_READ(otherHints);OWNER_READ(errors);
            OWNER_READ(hintEligible);OWNER_READ(hintAttempted);OWNER_READ(hintUnsupported);OWNER_READ(hintErrors);OWNER_READ(hintBudgetExhausted);OWNER_READ(hintRaces);OWNER_READ(ifErrors);
#undef OWNER_READ
            for(unsigned k=0;k<OwnerWaitKinds;++k){o.kind[k]=__atomic_load_n(&c.kind[k],__ATOMIC_RELAXED);out->saturated|=o.kind[k]==~uint64_t(0);}
        }
    }
};
void SchedulerReadyLockOwnerMode(bool enabled);
bool SchedulerReadyLockYieldMode(bool enabled);
void SchedulerReadyLockOwnerRead(ReadyLockOwnerSnapshot* out);
}
