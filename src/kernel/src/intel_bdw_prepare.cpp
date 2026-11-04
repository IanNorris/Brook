#include "intel_bdw_oneshot.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"

namespace brook {
namespace {
// Preparation only: the addresses below are placeholders for a future DMA
// mapping. No GGTT reservation, hardware access or GPU submission occurs.
struct PrivatePages {
    bool alloc(BdwOwnedPage& out) {
        auto p=PmmAllocPage(MemTag::KernelData,KernelPid);
        if(!p.raw())return false;
        // The boot direct map used here covers only the first 16 GiB.
        if(p.raw()>=(uint64_t{16}<<30)){PmmFreePage(p);return false;}
        out={p.raw(),reinterpret_cast<uint8_t*>(PhysToVirt(p).raw())};return true;
    }
    void free(BdwOwnedPage p){PmmFreePage(PhysicalAddress(p.dma));}
};
}
bool BdwPreparePrivateImages() {
    PrivatePages pages;BdwBcsOneShot image;uint32_t staging[2048]{};
    if(!image.Prepare(pages,65536,1,0x42524f4f,staging))return false;
    return image.ReleasePrepared(pages);
}
}
