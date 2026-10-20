#pragma once
#include <stdint.h>

namespace brook {
// Failure reserves no ticket. After success, confirm ownership before entering:
// an unreserved reader can span a full uint32 counter cycle while descheduled.
// Rare stale success waits for its already-reserved ticket using the caller's
// ordinary bounded wait; never acquire again or abandon that reservation.
struct TicketNoHook {void operator()()const{}};
struct TicketReservationNoHook {void operator()(uint32_t)const{}};
template<class Wait,class BeforeCas=TicketNoHook,class Reserved=TicketReservationNoHook>
inline bool TicketTryClaim(volatile uint32_t* next,volatile uint32_t* serving,Wait wait,BeforeCas beforeCas={},Reserved reserved={}){
    auto expected=__atomic_load_n(serving,__ATOMIC_ACQUIRE);
    const auto ticket=expected;beforeCas();
    if(!__atomic_compare_exchange_n(next,&expected,ticket+1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return false;
    reserved(ticket);
    if(__atomic_load_n(serving,__ATOMIC_ACQUIRE)!=ticket)wait(ticket);
    return true;
}
inline bool IrqAccountingMayDefer(bool enabled,unsigned cpu,bool periodic,bool user,bool current,bool idle){
    return enabled && cpu!=0 && periodic && !user && current && !idle;
}
struct IrqAccountingSnapshot {
    bool enabled=false;
    unsigned cpus=0;
    struct Cpu {uint64_t attempts=0,successes=0,deferred=0,ifErrors=0,staleTicketWaits=0;} cpu[64]{};
};
void SchedulerIrqAccountingMode(bool enabled);
void SchedulerIrqAccountingRead(IrqAccountingSnapshot* output);
}
