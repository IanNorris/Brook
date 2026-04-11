#include "compositor.h"
#include "process.h"
#include "tty.h"
#include "serial.h"
#include "memory/virtual_memory.h"
#include "memory/address.h"

namespace brook {

// Cached physical framebuffer info.
static volatile uint32_t* g_physFb       = nullptr;
static uint32_t           g_physFbWidth  = 0;
static uint32_t           g_physFbHeight = 0;
static uint32_t           g_physFbStride = 0; // in pixels

// Registered process slots for compositing.
static constexpr uint32_t MAX_COMPOSITED = 32;
static Process* g_compositedProcs[MAX_COMPOSITED] = {};
static uint32_t g_compositedCount = 0;

// Composite every N ticks (1ms each). 16 ≈ 60fps, 33 ≈ 30fps.
static constexpr uint32_t COMPOSITE_INTERVAL = 16;
static uint32_t g_tickCounter = 0;

void CompositorInit()
{
    uint32_t* pixels;
    uint32_t w, h, strideBytes;
    if (!TtyGetFramebuffer(&pixels, &w, &h, &strideBytes))
    {
        SerialPuts("COMPOSITOR: no framebuffer available\n");
        return;
    }

    g_physFb       = reinterpret_cast<volatile uint32_t*>(pixels);
    g_physFbWidth  = w;
    g_physFbHeight = h;
    g_physFbStride = strideBytes / 4; // convert byte stride to pixel stride

    SerialPrintf("COMPOSITOR: initialised, %ux%u stride=%u\n",
                 g_physFbWidth, g_physFbHeight, g_physFbStride);
}

bool CompositorSetupProcess(Process* proc, int16_t destX, int16_t destY, uint8_t scale)
{
    if (!g_physFb || !proc) return false;
    if (scale == 0)
    {
        // No compositing — process writes directly to physical FB.
        proc->fbVirtual     = nullptr;
        proc->fbVirtual     = nullptr;
        proc->fbVirtualSize = 0;
        proc->fbDestX       = 0;
        proc->fbDestY       = 0;
        proc->fbScale       = 0;
        return true;
    }

    // Allocate a virtual framebuffer the same size as the physical one.
    // This way the process sees xres/yres matching the real screen and
    // its internal scaling logic works unmodified.
    uint64_t fbSizeBytes = static_cast<uint64_t>(g_physFbStride) * g_physFbHeight * 4;
    uint64_t fbPages     = (fbSizeBytes + 4095) / 4096;

    // Allocate physical pages for the virtual framebuffer.
    VirtualAddress vfbAddr = VmmAllocPages(fbPages, VMM_WRITABLE, MemTag::Device, proc->pid);
    if (!vfbAddr)
    {
        SerialPrintf("COMPOSITOR: failed to alloc %lu pages for virtual fb\n", fbPages);
        return false;
    }

    auto* vfbPtr = reinterpret_cast<uint32_t*>(vfbAddr.raw());

    // Clear the virtual framebuffer to black.
    for (uint64_t i = 0; i < (fbSizeBytes / 4); ++i)
        vfbPtr[i] = 0;

    proc->fbVirtual     = vfbPtr;
    proc->fbVirtualSize = static_cast<uint32_t>(fbSizeBytes);
    proc->fbDestX       = destX;
    proc->fbDestY       = destY;
    proc->fbScale       = scale;

    // Register for compositing.
    if (g_compositedCount < MAX_COMPOSITED)
        g_compositedProcs[g_compositedCount++] = proc;

    SerialPrintf("COMPOSITOR: proc '%s' pid %u → vfb at 0x%lx, dest=(%d,%d) scale=%u\n",
                 proc->name, proc->pid, vfbAddr.raw(),
                 destX, destY, scale);
    return true;
}

// Blit a single process's virtual FB onto the physical FB.
static void BlitProcess(Process* proc)
{
    if (!proc->fbVirtual || proc->fbScale == 0) return;

    const uint32_t* src = proc->fbVirtual;
    const uint32_t  srcW = g_physFbWidth;
    const uint32_t  srcH = g_physFbHeight;
    const uint32_t  srcStride = g_physFbStride;

    const int scale = proc->fbScale;
    const int dstX0 = proc->fbDestX;
    const int dstY0 = proc->fbDestY;

    // Destination dimensions after downscale.
    const uint32_t dstW = srcW / static_cast<uint32_t>(scale);
    const uint32_t dstH = srcH / static_cast<uint32_t>(scale);

    for (uint32_t dy = 0; dy < dstH; ++dy)
    {
        int physY = dstY0 + static_cast<int>(dy);
        if (physY < 0) continue;
        if (static_cast<uint32_t>(physY) >= g_physFbHeight) break;

        // Source row — nearest-neighbour sampling.
        uint32_t srcY = dy * static_cast<uint32_t>(scale);
        if (srcY >= srcH) break;
        const uint32_t* srcRow = src + srcY * srcStride;

        volatile uint32_t* dstRow = g_physFb + static_cast<uint32_t>(physY) * g_physFbStride;

        for (uint32_t dx = 0; dx < dstW; ++dx)
        {
            int physX = dstX0 + static_cast<int>(dx);
            if (physX < 0) continue;
            if (static_cast<uint32_t>(physX) >= g_physFbWidth) break;

            uint32_t srcX = dx * static_cast<uint32_t>(scale);
            if (srcX >= srcW) break;

            dstRow[physX] = srcRow[srcX];
        }
    }
}

void CompositorTick()
{
    if (!g_physFb || g_compositedCount == 0)
        return;

    if (++g_tickCounter < COMPOSITE_INTERVAL)
        return;
    g_tickCounter = 0;

    // Ensure we're using the kernel page table for reading virtual FBs.
    // The timer may fire while a process CR3 is loaded, but kernel
    // higher-half pages are shared so this should be safe.
    for (uint32_t i = 0; i < g_compositedCount; ++i)
    {
        Process* p = g_compositedProcs[i];
        if (!p || p->state == ProcessState::Terminated)
            continue;
        if (!p->fbVirtual || p->fbScale == 0)
            continue;
        BlitProcess(p);
    }
}

} // namespace brook
