#pragma once
#include <stdint.h>

namespace brook {
// Rolling cumulative counters for kernel virtual mappings allocated by the DRM
// shim. Releasing a kernel mapping does not prove physical-page release: user
// mappings may still pin pages. These are not host VRAM or all GPU allocations.
struct DrmBackingDiagnostics {
    uint64_t allocatedMappings, allocatedPageBytes;
    uint64_t releasedMappings, releasedPageBytes;
    uint64_t gemCloseDiscardedMappings, gemCloseDiscardedPageBytes;
    uint64_t failedAllocations, failedRequestedBytes;
};
DrmBackingDiagnostics DrmBackingDiagnosticsRead();
}
