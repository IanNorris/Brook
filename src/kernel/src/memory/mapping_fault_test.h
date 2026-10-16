#pragma once
#include "address_space.h"

namespace brook {
#ifdef BROOK_MAPPING_TEST_HOOKS
// Test kernels only. Called outside AS/PT/backing locks, with the real fault's
// MappingTransaction still owning its cancellation region and backing snapshot.
bool MappingFaultTestBarrier(AddressSpaceState* space,uint64_t page,uint32_t phase);
// Pause at compositor frame entry, outside all renderer/source locks. Holds no
// process/AS reference, so consuming wait must complete while epochs stop.
bool MappingRendererTestBarrier();
int64_t MappingFaultTestControl(uint64_t action,uint64_t address,uint64_t length,
                                uint64_t phase,uint64_t quota,uint64_t);
#else
inline bool MappingFaultTestBarrier(AddressSpaceState*,uint64_t,uint32_t) { return true; }
inline bool MappingRendererTestBarrier() { return true; }
#endif
}
