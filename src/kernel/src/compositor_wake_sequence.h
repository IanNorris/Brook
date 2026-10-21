#pragma once
#include <stdint.h>
namespace brook {
// A scheduler wake latch belongs to its next block, so an intermediate GPU or
// mutex wait may consume it. Frame work needs a separate durable generation.
struct CompositorWakeSequence {
    uint64_t sequence=0;
    uint64_t Snapshot()const{return __atomic_load_n(&sequence,__ATOMIC_ACQUIRE);}
    void Publish(){
        uint64_t old=__atomic_load_n(&sequence,__ATOMIC_RELAXED);
        while(old!=~uint64_t(0) && !__atomic_compare_exchange_n(&sequence,&old,old+1,
                true,__ATOMIC_RELEASE,__ATOMIC_RELAXED)){}
    }
    bool Changed(uint64_t begin)const {
        const uint64_t now=Snapshot();
        // Exhaustion must not make an unobserved wake indistinguishable.
        return now!=begin || now==~uint64_t(0);
    }
};
}
