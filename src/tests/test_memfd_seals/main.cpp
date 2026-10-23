#include <cassert>
#include <cstdio>
#include "../../kernel/src/memfd_seals.h"

int main() {
    using namespace brook;
    MemFdSealState initial;
    assert(initial.bits==1 && !initial.mappings);
    assert(MemFdAddSeals(initial,2,true)==-1);
    for(unsigned bits=0;bits<16;++bits) {
        MemFdSealState state{0,0};
        assert(!MemFdAddSeals(state,bits,true)&&state.bits==bits);
        assert(MemFdSealedWrite(state,10,5)==((bits&8)?-1:0));
        assert(MemFdSealedWrite(state,10,11)==((bits&12)?-1:0));
        assert(MemFdSealedResize(state,10,5)==((bits&2)?-1:0));
        assert(MemFdSealedResize(state,10,15)==((bits&4)?-1:0));
        assert(!MemFdSealedResize(state,10,10));
    }
    MemFdSealState state{0,1};
    assert(MemFdAddSeals(state,15,true)==-16 && state.bits==0); // no partial SEAL publication
    assert(MemFdAddSeals(state,0x80000000u,true)==-22 && state.bits==0);
    assert(MemFdAddSeals(state,2,false)==-1 && state.bits==0);
    assert(MemFdSealedResize(state,10,5)==-16);
    state.mappings=0;
    assert(!MemFdAddSeals(state,8,true));
    uint32_t maximum=7;
    assert(MemFdAdmitMapping(state,true,2,&maximum)==-1 && !state.mappings && maximum==7);
    assert(!MemFdAdmitMapping(state,true,1,&maximum)&&maximum==5&&state.mappings==1);
    assert(!MemFdAddSeals(state,8,true)); // an already-present WRITE seal is idempotent
    assert(!MemFdAddSeals(state,7,true)&&state.bits==15);
    assert(MemFdAddSeals(state,0,true)==-1);
    state={0,~uint64_t{0}};maximum=7;
    assert(MemFdAdmitMapping(state,false,1,&maximum)==-75 && maximum==7 && state.mappings==~uint64_t{0});
    std::puts("MEMFD_SEALS_POLICY_PASS");
}
