#pragma once
#include <stdint.h>

namespace brook {
// Prepare the actual pinned status page after engine reset and before the
// first submission. No other CPU or GPU writer may access these fields. The
// caller supplies the platform-specific cache publication operation; failure
// leaves the page and any existing binding retained by its owner.
template<class Publish> bool BdwCsbInitializeForReset(uint8_t* page,Publish publish) {
    const auto p=reinterpret_cast<uintptr_t>(page);
    if(!p || (p&4095) || p>~uintptr_t(0)-4096)return false;
    for(unsigned i=0x40;i<0x70;++i)page[i]=0xff;
    __atomic_store_n(reinterpret_cast<uint32_t*>(page+0x7c),5u,__ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return publish(page);
}
// Gen8 engine status-page CPU reads. The caller must pin the exact owned HWS
// mapping and establish device-to-CPU visibility before each producer sample;
// LFENCE orders subsequent loads but cannot make noncoherent DMA coherent.
// No CPU writer may modify this device-owned area while it is being sampled.
inline bool BdwCsbReadProducer(const uint8_t* page,uint32_t& producer) {
    const auto p=reinterpret_cast<uintptr_t>(page);
    if(!p || (p&4095) || p>~uintptr_t(0)-4096)return false;
    // Only the low byte is the hardware producer; upper bytes are reserved.
    // Retain all eight low bits so the consumer can reject invalid values.
    producer=__atomic_load_n(reinterpret_cast<const uint32_t*>(page+0x7c),__ATOMIC_RELAXED)&0xffu;
    __asm__ volatile("lfence":::"memory");
    return true;
}
// Call only after BdwCsbReadProducer in the same CPU-pinned observation.
inline bool BdwCsbReadRecord(const uint8_t* page,unsigned slot,uint64_t& record) {
    const auto p=reinterpret_cast<uintptr_t>(page);
    if(!p || (p&4095) || p>~uintptr_t(0)-4096 || slot>=6)return false;
    record=__atomic_load_n(reinterpret_cast<const uint64_t*>(page+0x40+8*slot),__ATOMIC_RELAXED);
    return true;
}
}
