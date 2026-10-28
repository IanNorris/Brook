#include <cassert>
#include "../../kernel/src/memory/fault_frame_reserve.h"
using namespace brook;
int main() {
    FaultFrameReserve reserve;assert(!reserve.Pop());assert(!reserve.Push(PhysicalAddress{}));
    assert(reserve.Push(PhysicalAddress(4096)));assert(!reserve.Push(PhysicalAddress(4096)));
    for(unsigned i=1;i<FaultFrameReserve::Capacity;++i)assert(reserve.Push(PhysicalAddress((i+1)*4096)));
    assert(reserve.Size()==FaultFrameReserve::Capacity);assert(!reserve.Push(PhysicalAddress(40960)));
    for(unsigned iteration=0;iteration<1000;++iteration) {
        auto frame=reserve.Pop();assert(frame && reserve.Size()==FaultFrameReserve::Capacity-1);
        assert(reserve.Push(frame));assert(reserve.Size()==FaultFrameReserve::Capacity);
    }
    for(unsigned i=0;i<FaultFrameReserve::Capacity;++i)assert(reserve.Pop());
    assert(!reserve.Pop() && !reserve.Size());
}
