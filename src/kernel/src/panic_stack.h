#pragma once
#include <stdint.h>

namespace brook {
// A conservative window derived from the current process's kernel stack.
// It does not prove that page tables remain healthy after memory corruption.
struct PanicStackBounds {
    uint64_t base=0,top=0;
    bool Contains(uint64_t address,uint64_t bytes) const {
        return base>=0xffff800000000000ULL && top>base && bytes &&
               address>=base && address<top && bytes<=top-address;
    }
    uint32_t Available(uint64_t address,uint32_t limit) const {
        if(!Contains(address,1))return 0;
        const uint64_t left=top-address;
        return left<limit?static_cast<uint32_t>(left):limit;
    }
};
PanicStackBounds PanicCurrentStackBounds();
}
