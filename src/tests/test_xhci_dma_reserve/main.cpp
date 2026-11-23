#include <cassert>
#include <cstdio>
#include "xhci_dma_reserve.h"
using namespace brook;

int main() {
    XhciDmaReserve<2048> r; r.init(2048);
    // A KVM reconnect of an 8-device tree, ~8 pages per device, repeated
    // many times: with release on detach, usage never grows.
    for (int cycle = 0; cycle < 1000; ++cycle) {
        int got[64]; unsigned n = 0;
        for (int d = 0; d < 8; ++d) {
            got[n++] = r.alloc(1); got[n++] = r.alloc(2); got[n++] = r.alloc(2);
            got[n++] = r.alloc(1); got[n++] = r.alloc(1); got[n++] = r.alloc(1);
        }
        for (unsigned i = 0; i < n; ++i) assert(got[i] >= 0);
        static const unsigned sizes[6] = {1, 2, 2, 1, 1, 1};
        for (unsigned i = 0; i < n; ++i) assert(r.release(uint32_t(got[i]), sizes[i % 6]));
        assert(r.inUse == 0);
    }
    assert(r.peak == 64);
    // Exhaustion, then reuse of a freed hole.
    XhciDmaReserve<8> s; s.init(8);
    int a = s.alloc(4), b = s.alloc(4);
    assert(a == 0 && b == 4 && s.alloc(1) == -1);
    assert(s.release(0, 4));
    assert(s.alloc(2) == 0 && s.alloc(2) == 2 && s.alloc(1) == -1);
    // Fragmentation: a 2-page run needs two adjacent free pages.
    assert(s.release(0, 2) && s.release(4, 4));
    assert(s.alloc(3) == 4);
    // Invalid frees change nothing.
    assert(!s.release(0, 3));          // page 2 is free... (0,1 free too) -> not all allocated
    assert(!s.release(7, 2) && !s.release(9, 1) && !s.release(0, 0));
    const uint32_t before = s.inUse;
    assert(!s.release(1, 1) && s.inUse == before); // double free refused
    puts("test_xhci_dma_reserve: ok");
    return 0;
}
