#include "usb_ecm_rx_credits.h"
#include <cassert>
#include <cstdint>
#include <initializer_list>

int main()
{
    constexpr unsigned Depth=UsbEcmRxCredits::Slots;
    constexpr unsigned Capacity=UsbEcmRxCredits::Capacity;
    constexpr unsigned UsableTrbs=UsbEcmRxCredits::RingEntries-1;
    constexpr uint64_t Base=0x1000;
    UsbEcmRxCredits credits{};
    credits.ringBase=Base;
    for(unsigned i=0;i<Depth;++i){
        assert(credits.CanArm(i,Depth,Capacity-Depth));
        credits.Arm(i,Base+i*16);
    }
    assert(credits.outstanding==Depth);
    for(unsigned error:{0u,2u,3u,4u,6u,255u}){
        assert(credits.CompletedSlot(Base,error,0)==-1);
        assert(credits.outstanding==Depth);
    }
    assert(credits.CompletedSlot(Base,1,UsbEcmRxCredits::BufferBytes+1)==-1);
    assert(credits.CompletedSlot(Base-16,1,0)==-1);
    assert(credits.CompletedSlot(Base+1,1,0)==-1);
    assert(credits.CompletedSlot(Base+UsableTrbs*16,1,0)==-1);
    assert(credits.CompletedSlot(Base,13,UsbEcmRxCredits::BufferBytes)==0);
    for(unsigned i=0;i<Depth;++i){
        const auto token=Base+i*16;
        assert(credits.CompletedSlot(token,1,0)==static_cast<int>(i));
        credits.Complete(i);
        assert(credits.Find(token)==-1);
        assert(!credits.CanArm(i,Depth,Capacity-Depth+i+1));
    }
    assert(credits.outstanding==0);
    assert(!credits.CanArm(0,Depth,Capacity));
    assert(credits.CanArm(0,Depth,Capacity-1));
    assert(!credits.CanArm(1,1,0));
    assert(!credits.CanArm(0,Depth+1,0));

    UsbEcmRxPressure pressure{};
    pressure.Observe(Depth,Depth,Capacity-Depth);
    assert(!pressure.blocked && pressure.entries==0);
    pressure.Observe(Depth,Depth-1,Capacity-Depth+1);
    assert(pressure.blocked && pressure.entries==1 && !pressure.zero);
    pressure.Observe(Depth,0,Capacity);
    assert(pressure.zero && pressure.zeroEntries==1);
    pressure.Observe(Depth,1,Capacity-1);
    assert(!pressure.zero && pressure.entries==1);
    pressure.Observe(Depth,Depth,0);
    assert(!pressure.blocked);
    pressure.Observe(Depth,0,Capacity);
    assert(pressure.entries==2 && pressure.zeroEntries==2);

    // Keep a full flight live through several transfer-ring wraps. Reuse
    // each completed DMA slot with a different ownership token.
    for(unsigned n=0;n<Depth;++n)credits.Arm(n,Base+n*16);
    for(unsigned n=0;n<4096;++n){
        const unsigned slot=n%Depth;
        const auto token=Base+(n%UsableTrbs)*16;
        const auto next=Base+((n+Depth)%UsableTrbs)*16;
        assert(credits.CompletedSlot(token,13,UsbEcmRxCredits::BufferBytes)==static_cast<int>(slot));
        credits.Complete(slot);
        assert(credits.Find(token)==-1 && credits.Find(next)==-1);
        assert(credits.CanArm(slot,Depth,0));
        credits.Arm(slot,next);
        assert(credits.outstanding==Depth && credits.Find(token)==-1);
    }
}
