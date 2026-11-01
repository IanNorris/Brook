#include "usb_ecm_trb_ring.h"
#include "xhci_completion_queue.h"
#include <cassert>
#include <cstdint>

struct alignas(16) Trb { uint64_t param; uint32_t status,control; };

int main() {
    constexpr unsigned n=8;
    Trb ring[n]{}; brook::XhciInitializeLink(ring[n-1],0x1000);
    uint32_t enqueue=0,consumer=0; bool producerCycle=true,consumerCycle=true;
    unsigned wraps=0;
    auto tryConsume=[&]() {
        auto control=__atomic_load_n(&ring[consumer].control,__ATOMIC_ACQUIRE);
        if (bool(control&1u)!=consumerCycle) return false;
        if (((control>>10)&63u)==6) {
            assert(consumer==n-1 && (control&2u));
            consumer=0;consumerCycle=!consumerCycle;
            control=__atomic_load_n(&ring[consumer].control,__ATOMIC_ACQUIRE);
            if (bool(control&1u)!=consumerCycle) return false;
        }
        assert(((control>>10)&63u)==1);
        ++consumer;return true;
    };
    for (unsigned i=0;i<256;++i) {
        auto token=UsbEcmPrepareTrb(ring,n,0x1000,enqueue,producerCycle,true,
                                    0x10000+i*2048,2048,false,false,[&] {
            ++wraps;
            // Consumer traverses a newly owned Link before the producer has
            // prepared ring[0]; it must stop on the previous-lap cycle.
            assert(!tryConsume());
        });
        assert(!tryConsume()); // prepared Normal is still unowned
        auto index=(token-0x1000)/sizeof(Trb);
        assert(index<n-1 && ring[index].param==0x10000+i*2048);
        __atomic_fetch_xor(&ring[index].control,1u,__ATOMIC_RELEASE);
        assert(tryConsume());
    }
    assert(wraps>30);

    // TX exact-packet data plus a distinct zero-length TD crosses the Link.
    // The first TD stays unowned until the following TD is ready.
    Trb tx[n]{}; brook::XhciInitializeLink(tx[n-1],0x2000);
    uint32_t txHead=n-2;bool txCycle=true;
    auto first=UsbEcmPrepareTrb(tx,n,0x2000,txHead,txCycle,false,
                                0x50000,1024,false,false,[]{});
    assert(first==0x2000+(n-2)*sizeof(Trb));
    assert(!(tx[n-2].control&1u) && (tx[n-2].control&32u));
    auto final=UsbEcmPrepareTrb(tx,n,0x2000,txHead,txCycle,false,
                                0x50000,0,false,true,[]{});
    assert(final==0x2000 && !(tx[n-1].control&16u));
    assert((tx[n-1].control&1u) && !(tx[0].control&1u));
    assert((tx[0].control&32u) && tx[0].status==0);
    __atomic_fetch_xor(&tx[n-2].control,1u,__ATOMIC_RELEASE);
    assert(tx[n-2].control&1u);

    Trb immediate[n]{};brook::XhciInitializeLink(immediate[n-1],0x3000);
    uint32_t immediateHead=0;bool immediateCycle=true;
    auto single=UsbEcmPrepareTrb(immediate,n,0x3000,immediateHead,immediateCycle,
                                 false,0x70000,100,false,true,[]{});
    assert(single==0x3000 && (immediate[0].control&1u));
    assert((immediate[0].control&32u) && !(immediate[0].control&16u));
    return 0;
}
