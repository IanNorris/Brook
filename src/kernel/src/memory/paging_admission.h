#pragma once
#include <stdint.h>
namespace brook {
// Conservative experimental admission, independent of AS lifetime references.
// Ever-shared address spaces cannot opt into single-task paging; once opted in,
// sharing stays forbidden until exec creates a fresh AS, even after unmapping.
// The mapping caller also holds AS.mutation against fork's source snapshot.
struct PagingAdmission {
    uint32_t state=0; // 0 never shared; 1 sharing admitted; 2 exclusive paging
    bool Share() {
        uint32_t old=0;
        return __atomic_compare_exchange_n(&state,&old,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE) || old==1;
    }
    bool ClaimExclusive() {
        uint32_t old=0;
        return __atomic_compare_exchange_n(&state,&old,2,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE) || old==2;
    }
    bool Exclusive() const { return __atomic_load_n(&state,__ATOMIC_ACQUIRE)==2; }
};
}
