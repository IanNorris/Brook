#pragma once
#include <stdint.h>
namespace brook {
// A second, non-sleeping ownership boundary around the AHCI command and DMA
// buffer. Ordinary callers must already hold their existing sleepable mutex.
// A panic may claim an idle gate once, without waiting for a stopped owner.
// Once claimed it stays closed to ordinary I/O until reboot, even on error.
class AhciPanicGate {
    // 0 idle, 1 normal owner, 2 panic owner, 3 normal owner with panic pending.
    uint32_t owner_=0;
public:
    enum class Request { Acquired, Pending, Unavailable };
    bool EnterNormal() {
        uint32_t expected=0;
        return __atomic_compare_exchange_n(&owner_,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
    }
    void LeaveNormal() {
        uint32_t expected=1;
        if(!__atomic_compare_exchange_n(&owner_,&expected,0,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED) && expected==3)
            __atomic_store_n(&owner_,2,__ATOMIC_RELEASE);
    }
    bool EnterPanic() {
        uint32_t expected=0;
        return __atomic_compare_exchange_n(&owner_,&expected,2,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
    }
    Request RequestPanic() {
        uint32_t current=__atomic_load_n(&owner_,__ATOMIC_ACQUIRE);
        for(unsigned attempt=0;attempt<64;++attempt) {
            if(current!=0 && current!=1)return Request::Unavailable;
            const uint32_t desired=current==0?2:3;
            if(__atomic_compare_exchange_n(&owner_,&current,desired,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
                return desired==2?Request::Acquired:Request::Pending;
        }
        return Request::Unavailable;
    }
    bool PanicOwned() const {return __atomic_load_n(&owner_,__ATOMIC_ACQUIRE)==2;}
    // Normal owner checks this between commands, never while the controller
    // may still be using its DMA descriptors or data buffer.
    bool PanicPending() const {return __atomic_load_n(&owner_,__ATOMIC_ACQUIRE)==3;}
#if defined(BROOK_AHCI_PANIC_CONTENTION_TEST)
    bool NormalWithPanicPending() const {return __atomic_load_n(&owner_,__ATOMIC_ACQUIRE)==3;}
#endif
};

// Allow an in-flight command on another CPU time to finish before halting
// peers. The ACPI PM timer's low 24 bits run at 3,579,545 Hz; two seconds
// fits within one wrap. The independent poll limit bounds a stalled timer.
// This never takes ownership away from a normal caller.
template<class Io> bool AhciWaitForPanicOwner(AhciPanicGate& gate,Io& io,uint32_t pollLimit) {
    constexpr uint32_t mask=0xffffffu,twoSeconds=7159090u;
    const uint32_t start=io.readTimer()&mask;
    for(uint32_t n=0;n<pollLimit;++n) {
        if(gate.PanicOwned())return true;
        if(((io.readTimer()-start)&mask)>=twoSeconds)break;
        io.pause();
    }
    return gate.PanicOwned();
}
}
