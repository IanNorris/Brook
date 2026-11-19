#pragma once
#include <stdint.h>

namespace brook {
// Diagnostic charge independent of the three-bit PTE MemTag. Each physical
// allocation carries one class until its last reference is retired.
enum class PmmOwner : uint8_t {
    Automatic,
    KernelHeap, FileCache, FsBuffers, GpuBuffer,
    XhciDma, AhciDma, HdaDma, NetworkDma, DriverDma,
    PageTables, KernelStacks, User, Scanout, Shmem, Other, ExecCache,
    Count,
};
inline constexpr unsigned PmmOwnerCount = static_cast<unsigned>(PmmOwner::Count);
inline constexpr const char* PmmOwnerNames[PmmOwnerCount] = {
    "automatic", "kernel_heap", "file_cache", "fs_buffers", "gpu_buffers",
    "xhci_dma", "ahci_dma", "hda_dma", "network_dma", "driver_dma",
    "page_tables", "kernel_stacks", "user", "scanout", "shmem", "other", "exec_cache",
};
struct PmmOwnerStats {
    uint64_t pages[PmmOwnerCount]{};
    uint64_t ramPages = 0, freePages = 0, quarantinedPages = 0;
    uint64_t idleExecCachePages = 0;
    uint64_t Pages(PmmOwner owner) const { return pages[static_cast<unsigned>(owner)]; }
    uint64_t CachedPages() const { return Pages(PmmOwner::FileCache) + Pages(PmmOwner::ExecCache); }
    uint64_t AvailablePages() const { return freePages + idleExecCachePages; }
};
PmmOwnerStats PmmReadOwnerStats();
} // namespace brook
