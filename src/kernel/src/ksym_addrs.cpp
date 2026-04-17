#include "ksym_addrs.h"

namespace brook {

// g_ksymAddrTable and g_ksymAddrCount are defined in the generated file
extern const KsymAddrEntry g_ksymAddrTable[];
extern const uint32_t g_ksymAddrCount;

bool KsymFindByAddr(uint64_t addr, const char** outName, uint64_t* outOffset)
{
    if (g_ksymAddrCount == 0) return false;

    // Binary search for the largest entry <= addr
    uint32_t lo = 0, hi = g_ksymAddrCount;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_ksymAddrTable[mid].addr <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }

    if (lo == 0) return false; // addr is before first symbol

    lo--; // lo now points to the largest entry <= addr
    *outName = g_ksymAddrTable[lo].name;
    *outOffset = addr - g_ksymAddrTable[lo].addr;
    return true;
}

const KsymAddrEntry* KsymGetAddrTable(uint32_t* outCount)
{
    *outCount = g_ksymAddrCount;
    return g_ksymAddrTable;
}

} // namespace brook
