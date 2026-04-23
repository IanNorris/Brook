#pragma once
#include <stdint.h>

namespace brook {

struct KsymAddrEntry {
    uint64_t addr;
    const char* name;
};

// Reverse-lookup: find the symbol containing `addr`.
// Returns true if found, sets *outName and *outOffset.
// Uses binary search on the sorted g_ksymAddrTable.
bool KsymFindByAddr(uint64_t addr, const char** outName, uint64_t* outOffset);

// Get the symbol table for enumeration.
const KsymAddrEntry* KsymGetAddrTable(uint32_t* outCount);

} // namespace brook
