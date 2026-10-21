#pragma once
#include <stdint.h>

namespace brook {
enum class OwnerWaitObservation : unsigned {
    MissingOwner, TicketMismatch, Changed, Self, MissingSteal, Preempted, NotPreempted, Count
};
constexpr unsigned OwnerWaitKinds=unsigned(OwnerWaitObservation::Count);
struct ReadyLockOwnerCpu {
    uint64_t published=0,cleared=0,waits=0,observations=0,otherHints=0,errors=0;
    uint64_t kind[OwnerWaitKinds]{};
};
struct ReadyLockOwnerSnapshot {
    bool enabled=false,saturated=false;
    unsigned cpus=0;
    uint64_t owner=0;
    ReadyLockOwnerCpu cpu[64]{};
};
// Advisory publication only. The ticket lock remains the sole ownership and
// ordering authority. Publishers hold that lock, with IF clear; each CPU has
// one counter writer. All concurrent reader accesses are atomic.
class ReadyLockOwnerObservation {
    bool enabled_=false;
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
    bool Enabled()const{return __atomic_load_n(&enabled_,__ATOMIC_ACQUIRE);}
    void Mode(bool enabled){__atomic_store_n(&enabled_,enabled,__ATOMIC_RELEASE);}
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
    // Bounded sampling at exponentially spaced spin counts. No clock, CPUID,
    // allocation, hypercall, lock admission or wakeup operation occurs here.
    template<class Serving,class Hint> void Observe(unsigned cpu,Serving serving,Hint hint){
        if(cpu>=64 || !Enabled())return;
        auto& c=cpu_[cpu].value;Increment(c.observations);
        const uint32_t before=serving();const auto owner=Owner();
        OwnerWaitObservation result;
        if(!owner)result=OwnerWaitObservation::MissingOwner;
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
    }
    void Read(unsigned cpus,ReadyLockOwnerSnapshot* out)const{
        if(!out)return;*out={};out->enabled=Enabled();out->owner=Owner();out->cpus=cpus<64?cpus:64;
        for(unsigned i=0;i<out->cpus;++i){const auto& c=cpu_[i].value;auto& o=out->cpu[i];
#define OWNER_READ(f) o.f=__atomic_load_n(&c.f,__ATOMIC_RELAXED);out->saturated|=o.f==~uint64_t(0)
            OWNER_READ(published);OWNER_READ(cleared);OWNER_READ(waits);OWNER_READ(observations);OWNER_READ(otherHints);OWNER_READ(errors);
#undef OWNER_READ
            for(unsigned k=0;k<OwnerWaitKinds;++k){o.kind[k]=__atomic_load_n(&c.kind[k],__ATOMIC_RELAXED);out->saturated|=o.kind[k]==~uint64_t(0);}
        }
    }
};
void SchedulerReadyLockOwnerMode(bool enabled);
void SchedulerReadyLockOwnerRead(ReadyLockOwnerSnapshot* out);
}
