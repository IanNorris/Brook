#pragma once
#include <stdint.h>

namespace brook {
// The owning memfd lock serializes every policy operation and mapping lease.
// Descriptor references do not count as mappings: dup/SCM share these seals.
struct MemFdSealState {
    uint32_t bits=1; // F_SEAL_SEAL unless MFD_ALLOW_SEALING was requested
    uint64_t mappings=0;
};
inline int MemFdAddSeals(MemFdSealState& state,uint64_t additions,bool writable) {
    if(!writable)return -1;
    if(additions&~uint64_t{15})return -22;
    if(state.bits&1)return -1;
    // Conservative: even a read-only/private mapping delays first WRITE seal.
    if((additions&8) && !(state.bits&8) && state.mappings)return -16;
    state.bits|=static_cast<uint32_t>(additions);return 0;
}
inline int MemFdSealedWrite(const MemFdSealState& state,uint64_t size,uint64_t end) {
    return (state.bits&8) || ((state.bits&4) && end>size)?-1:0;
}
inline int MemFdSealedResize(const MemFdSealState& state,uint64_t oldSize,uint64_t newSize) {
    if((newSize<oldSize && (state.bits&2)) || (newSize>oldSize && (state.bits&4)))return -1;
    // Borrowed PTEs cannot survive freeing their backing pages. Coordinated
    // mapped shrink is unsupported until cross-address-space invalidation exists.
    if(newSize<oldSize && state.mappings)return -16;
    return 0;
}
inline int MemFdAdmitMapping(MemFdSealState& state,bool shared,uint32_t protection,uint32_t* maximum) {
    if(shared && (protection&2) && (state.bits&8))return -1;
    if(state.mappings==~uint64_t{0})return -75;
    if(shared && (state.bits&8))*maximum&=~uint32_t{2};
    ++state.mappings;return 0;
}
} // namespace brook
