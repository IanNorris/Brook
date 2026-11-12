#pragma once
#include <stdint.h>
namespace brook {
// Boot-lifetime one-shot admission. Failure is sticky because an incomplete
// device operation must not be replayed merely because its observer timed out.
class BdwBootstrapGate {
    uint32_t state_=0;
public:
    bool TryStart(){uint32_t expected=0;return __atomic_compare_exchange_n(&state_,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);}
    void Finish(bool ok){__atomic_store_n(&state_,ok?2u:3u,__ATOMIC_RELEASE);}
    uint32_t State()const{return __atomic_load_n(&state_,__ATOMIC_ACQUIRE);}
};
}
