#pragma once
#include "gpu_context.h"

namespace brook {
// Choose before creating a swapchain: missing fence/import support cannot be
// delegated to Mesa's optional ioctl probes, which may omit synchronization.
inline bool GpuVulkanDirectPresentation(bool gpuComposition,const GpuAppCapabilities& caps,bool imageImport) {
    return gpuComposition && imageImport && caps.contextInit && caps.resourceBlob &&
           caps.hostVisible && (caps.capsetMask&(uint64_t{1}<<4));
}
}
