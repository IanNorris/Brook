#pragma once
#include <stdint.h>
namespace brook {
// A one-shot install worker and shutdown share this gate. Once closing starts,
// no new worker is admitted; shutdown waits until a published worker finishes.
class SsdInstallGate {
    uint32_t state_=0;
public:
    bool Begin() {uint32_t expected=0;return __atomic_compare_exchange_n(&state_,&expected,6u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);}
    void Finish() {__atomic_fetch_and(&state_,~2u,__ATOMIC_RELEASE);}
    bool CloseReady() {return !(__atomic_fetch_or(&state_,1u,__ATOMIC_ACQ_REL)&2u);}
};
}
