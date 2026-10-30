#include "../../kernel/src/framebuffer_memory_type.h"
#include <cassert>
#include <initializer_list>
using namespace brook;
static FramebufferMtrrs initial() {
    FramebufferMtrrs m;m.capability=(1ull<<10)|2;m.defaults=(1ull<<11);m.physicalBits=36;m.count=2;return m;
}
static void region(FramebufferMtrrs& m,unsigned i,uint64_t begin,uint64_t size,unsigned type) {
    m.base[i]=begin|type;m.mask[i]=((~(size-1))&((1ull<<m.physicalBits)-1))|(1ull<<11);
}
int main() {
    constexpr uint64_t begin=0x80000000,end=begin+8192;
    auto m=initial();assert(FramebufferMtrrsAllowWc(m,begin,end)); // default UC + PAT WC = WC on Intel
    for(unsigned type: {0u,1u,4u,5u,6u}) {
        m=initial();m.defaults|=type;assert(FramebufferMtrrsAllowWc(m,begin,end));
    }
    m=initial();region(m,0,begin,1ull<<21,6);region(m,1,begin,1ull<<21,4);
    assert(FramebufferMtrrsAllowWc(m,begin,end)); // defined WT+WB overlap
    region(m,1,begin,1ull<<21,1);assert(!FramebufferMtrrsAllowWc(m,begin,end));
    region(m,0,begin,1ull<<21,0);assert(FramebufferMtrrsAllowWc(m,begin,end)); // UC wins
    m=initial();m.defaults=0;assert(!FramebufferMtrrsAllowWc(m,begin,end));
    m=initial();m.capability=0;assert(!FramebufferMtrrsAllowWc(m,begin,end));
    m=initial();m.physicalBits=64;assert(!FramebufferMtrrsAllowWc(m,begin,end));
    m=initial();m.count=33;assert(!FramebufferMtrrsAllowWc(m,begin,end));
    m=initial();region(m,0,begin,1ull<<21,6);m.mask[0]^=1ull<<24;assert(!FramebufferMtrrsAllowWc(m,begin,end));
    m=initial();region(m,0,begin+4096,1ull<<21,6);assert(!FramebufferMtrrsAllowWc(m,begin,end));
    m=initial();assert(!FramebufferMtrrsAllowWc(m,0x80000,0x81000));
    assert(!FramebufferMtrrsAllowWc(m,begin+1,end));
    auto copy=m;assert(FramebufferMtrrsEqual(m,copy));copy.fixed[3]^=1;assert(!FramebufferMtrrsEqual(m,copy));
    copy=m;copy.mask[0]^=1;assert(!FramebufferMtrrsEqual(m,copy));
}
