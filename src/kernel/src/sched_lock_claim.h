#pragma once
#include <stdint.h>

namespace brook {
// Unfair (test-and-test-and-set) acquisition on ticket-lock counters.
//
// A FIFO ticket lock hands ownership to one particular waiter. In a VM whose
// vCPUs the host deschedules, that waiter is often not running, and every
// other vCPU then spins until the host runs it again (lock-waiter preemption).
// With per-CPU timer ticks taking the lock, demand can exceed the handoff
// rate and the guest stops making progress. This claim only succeeds while no
// ticket is outstanding (next == serving), so whichever vCPU is running when
// the lock frees takes it. It interoperates with FIFO acquirers: while any
// FIFO ticket is outstanding the claim cannot succeed, and the FIFO order
// among those tickets is unchanged.
//
// After spinBudget failed checks the caller reserves an ordinary FIFO ticket,
// bounding starvation. A successful claim whose counter read was stale (a
// full uint32 cycle while this CPU was descheduled) holds a reserved ticket
// that is not yet current; it waits for that ticket like any FIFO acquirer.
// Returns the owned ticket. *contended is set when the first check failed.
template<class Spin,class WaitTicket>
inline uint32_t TicketUnfairAcquire(volatile uint32_t* next,volatile uint32_t* serving,uint64_t spinBudget,
                                    Spin spin,WaitTicket waitTicket,bool* contended,bool* fellBack=nullptr){
    uint64_t spins=0;
    for(;;){
        const uint32_t current=__atomic_load_n(serving,__ATOMIC_ACQUIRE);
        uint32_t expected=__atomic_load_n(next,__ATOMIC_RELAXED);
        if(expected==current &&
           __atomic_compare_exchange_n(next,&expected,current+1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)){
            if(__atomic_load_n(serving,__ATOMIC_ACQUIRE)!=current){*contended=true;waitTicket(current);}
            return current;
        }
        *contended=true;
        if(++spins>=spinBudget){
            if(fellBack)*fellBack=true;
            const uint32_t ticket=__atomic_fetch_add(next,1,__ATOMIC_RELAXED);
            waitTicket(ticket);
            return ticket;
        }
        spin(spins);
    }
}
}
