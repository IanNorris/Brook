#pragma once
#include <stdint.h>

// Caller serializes the producer and keeps the hardware consumer from
// overtaking live TDs. onWrap is a test seam immediately after Link ownership
// is published, before the next Normal TRB is prepared.
template<class Trb, class OnWrap>
uint64_t UsbEcmPrepareTrb(Trb* ring, uint32_t ringSize, uint64_t ringPhysical,
                          uint32_t& enqueue, bool& cycle, bool input,
                          uint64_t physical, uint32_t bytes, bool chain,
                          bool publish, OnWrap onWrap) {
    constexpr uint32_t Cycle=1u, ToggleCycle=2u, Isp=4u, Chain=16u, Ioc=32u;
    constexpr uint32_t Normal=1u<<10, Link=6u<<10;
    if (enqueue >= ringSize-1) {
        ring[ringSize-1].control=Link|ToggleCycle|(ring[ringSize-2].control&Chain)|(cycle?Cycle:0);
        cycle=!cycle; enqueue=0; onWrap();
    }
    auto& trb=ring[enqueue]; const auto token=ringPhysical+enqueue*sizeof(Trb);
    trb.param=physical; trb.status=bytes;
    const auto control=Normal|(input?Isp:0)|(chain?Chain:Ioc)|(cycle?Cycle:0);
    __atomic_store_n(&trb.control,control^(publish?0u:Cycle),__ATOMIC_RELEASE);
    ++enqueue; return token;
}
