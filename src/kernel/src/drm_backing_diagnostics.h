#pragma once
#include <stdint.h>

namespace brook {
// Rolling cumulative counters for kernel virtual mappings allocated by the DRM
// shim. Releasing a kernel mapping does not prove physical-page release: user
// mappings may still pin pages. These are not host VRAM or all GPU allocations.
// GEM-close counters count aliases, so shared backing can appear more than once.
// Quarantine is permanent until owned presentation/completion recovery exists.
struct DrmBackingDiagnostics {
    uint64_t allocatedMappings, allocatedPageBytes;
    uint64_t releasedMappings, releasedPageBytes;
    uint64_t gemCloseBackingAliases, gemCloseBackingAliasPageBytes;
    uint64_t failedAllocations, failedRequestedBytes;
    uint64_t quarantinedResources, quarantinedPageBytes;
    uint64_t failedRetirements;
};
DrmBackingDiagnostics DrmBackingDiagnosticsRead();
}
