#pragma once
#include <stdint.h>
namespace brook {
// A second, non-sleeping ownership boundary around the AHCI command and DMA
// buffer. Ordinary callers must already hold their existing sleepable mutex.
// A panic may claim an idle gate once, without waiting for a stopped owner.
// Once claimed it stays closed to ordinary I/O until reboot, even on error.
class AhciPanicGate {
    uint32_t owner_=0;
public:
    bool EnterNormal() {
        uint32_t expected=0;
        return __atomic_compare_exchange_n(&owner_,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
    }
    void LeaveNormal() { __atomic_store_n(&owner_,0,__ATOMIC_RELEASE); }
    bool EnterPanic() {
        uint32_t expected=0;
        return __atomic_compare_exchange_n(&owner_,&expected,2,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
    }
};
}
