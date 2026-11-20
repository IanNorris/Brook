#pragma once

// syscall_memory.h -- internal declarations shared from syscall_memory.cpp.
// Internal to the syscall layer; split from syscall.cpp.

#include "process.h"
#include "memory/mapping_fault.h"

namespace brook {

MappingFaultResult ResolveMappingFaultForSpace(Process* actor,AddressSpaceState* space,uint64_t address,uint64_t error,uint32_t retryBudget=0,bool allowPageableReclaim=false);

enum MmapFlags : uint64_t {
    MAP_SHARED    = 0x01,
    MAP_PRIVATE   = 0x02,
    MAP_FIXED     = 0x10,
    MAP_FIXED_NOREPLACE = 0x100000,
    MAP_ANONYMOUS = 0x20,
    MAP_32BIT     = 0x40,
    MAP_DENYWRITE = 0x0800,
    MAP_POPULATE  = 0x8000,
};

enum MmapProt : uint64_t {
    PROT_READ     = 0x1,
    PROT_WRITE    = 0x2,
    PROT_EXEC     = 0x4,
};

struct MappingBackingGuard {
    MappingBacking* value = nullptr;
    ~MappingBackingGuard() { MappingBackingUnref(value); }
};

} // namespace brook
