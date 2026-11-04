#include "intel_bdw_oneshot.h"
#include "intel_bdw_publish.h"
#include "memory/physical_memory.h"
#include "memory/virtual_memory.h"

namespace brook {
#ifdef BROOK_HOST_TEST
bool BdwTestDisableClflush();
#endif
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
    bool publish(BdwOwnedPage* pages,unsigned count) {
        // The diagnostic runs on private PMM pages and does not grant device
        // access. Pin execution while reading this CPU's CPUID and flushing.
#ifndef BROOK_HOST_TEST
        uint64_t flags;__asm__ volatile("pushfq; popq %0; cli":"=r"(flags)::"memory");
#endif
        uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
#ifdef BROOK_HOST_TEST
        if(BdwTestDisableClflush())d&=~(1u<<19);
#endif
        struct Flush {
            void fence(){__atomic_thread_fence(__ATOMIC_SEQ_CST);}
            void line(void* p){__asm__ volatile("clflush (%0)"::"r"(p):"memory");}
        } flush;
        const bool ok=(d&(1u<<19)) && BdwPublishCpuPages(flush,pages,count,((b>>8)&255)*8);
#ifndef BROOK_HOST_TEST
        if(flags&(1ull<<9))__asm__ volatile("sti":::"memory");
#endif
        return ok;
    }
};
}
bool BdwPreparePrivateImages() {
    PrivatePages pages;BdwBcsOneShot image;uint32_t staging[2048]{};
    if(!image.Prepare(pages,65536,1,0x42524f4f,staging))return false;
    const bool published=image.PublishPrepared(pages);
    return image.ReleasePrepared(pages) && published;
}
}
