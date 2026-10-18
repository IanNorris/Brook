#pragma once
#include <stdint.h>

namespace brook {
// Fake mmap offsets are opaque identities, not GEM slot numbers. One global
// allocator keeps old offsets distinct across slot and DRM-file reuse. Callers
// retain the selected GEM/backing under their own mapping lifetime protocol.
struct DrmMapTokenAllocator {
    static constexpr uint64_t First = 4096;
    static constexpr uint64_t Last = 0x7ffffffffffff000ULL;
    uint64_t next = First;

    uint64_t Allocate() {
        uint64_t value = __atomic_load_n(&next, __ATOMIC_RELAXED);
        for (;;) {
            if (value > Last) return 0; // saturated: never wrap/reissue an offset
            if (__atomic_compare_exchange_n(&next, &value, value + 4096, false,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                return value;
        }
    }
};

// Caller holds the DRM file's GEM/mapping lock. Dead slots and unissued offsets
// must not resolve, even when a new GEM has reused the same numeric handle.
template<class Gem>
Gem* DrmFindMapToken(Gem* gems, uint32_t count, uint64_t token) {
    if (!token || (token & 4095) || token > DrmMapTokenAllocator::Last) return nullptr;
    for (uint32_t i = 0; i < count; ++i)
        if (gems[i].used && gems[i].mapToken == token) return &gems[i];
    return nullptr;
}
}
