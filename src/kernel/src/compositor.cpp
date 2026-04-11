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
static constexpr uint32_t MAX_COMPOSITED = 64;
static Process* g_compositedProcs[MAX_COMPOSITED] = {};
static uint32_t g_compositedCount = 0;

// Composite every N ticks (1ms each). Higher = less CPU overhead.
static constexpr uint32_t COMPOSITE_INTERVAL = 100;
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

void CompositorGetPhysDims(uint32_t* w, uint32_t* h)
{
    if (w) *w = g_physFbWidth;
    if (h) *h = g_physFbHeight;
}

bool CompositorSetupProcess(Process* proc, int16_t destX, int16_t destY,
                             uint32_t vfbWidth, uint32_t vfbHeight, uint8_t scale)
{
    if (!g_physFb || !proc) return false;

    if (vfbWidth == 0 || vfbHeight == 0)
    {
        proc->fbVirtual     = nullptr;
        proc->fbVirtualSize = 0;
        proc->fbVfbWidth    = 0;
        proc->fbVfbHeight   = 0;
        proc->fbVfbStride   = 0;
        proc->fbDestX       = 0;
        proc->fbDestY       = 0;
        proc->fbScale       = 0;
        return true;
    }

    uint32_t stride = vfbWidth;
    uint64_t fbSizeBytes = static_cast<uint64_t>(stride) * vfbHeight * 4;
    uint64_t fbPages     = (fbSizeBytes + 4095) / 4096;

    VirtualAddress vfbAddr = VmmAllocPages(fbPages, VMM_WRITABLE, MemTag::Device, proc->pid);
    if (!vfbAddr)
    {
        SerialPrintf("COMPOSITOR: failed to alloc %lu pages for vfb %ux%u\n",
                     fbPages, vfbWidth, vfbHeight);
        return false;
    }

    auto* vfbPtr = reinterpret_cast<uint32_t*>(vfbAddr.raw());

    for (uint64_t i = 0; i < (fbSizeBytes / 4); ++i)
        vfbPtr[i] = 0;

    proc->fbVirtual     = vfbPtr;
    proc->fbVirtualSize = static_cast<uint32_t>(fbSizeBytes);
    proc->fbVfbWidth    = vfbWidth;
    proc->fbVfbHeight   = vfbHeight;
    proc->fbVfbStride   = stride;
    proc->fbDestX       = destX;
    proc->fbDestY       = destY;
    proc->fbScale       = (scale > 0) ? scale : 1;

    if (g_compositedCount < MAX_COMPOSITED)
        g_compositedProcs[g_compositedCount++] = proc;

    SerialPrintf("COMPOSITOR: proc '%s' pid %u → vfb %ux%u at 0x%lx, dest=(%d,%d) scale=%u\n",
                 proc->name, proc->pid, vfbWidth, vfbHeight, vfbAddr.raw(),
                 destX, destY, scale);
    return true;
}

// Blit a process's VFB onto the physical FB with optional downscaling.
static void BlitProcess(Process* proc)
{
    if (!proc->fbVirtual || proc->fbVfbWidth == 0) return;

    const uint32_t* src = proc->fbVirtual;
    const uint32_t  srcW = proc->fbVfbWidth;
    const uint32_t  srcH = proc->fbVfbHeight;
    const uint32_t  srcStride = proc->fbVfbStride;
    const uint32_t  scale = proc->fbScale;

    const int dstX0 = proc->fbDestX;
    const int dstY0 = proc->fbDestY;

    // Destination dimensions after downscale.
    const uint32_t dstW = srcW / scale;
    const uint32_t dstH = srcH / scale;

    for (uint32_t dy = 0; dy < dstH; ++dy)
    {
        int physY = dstY0 + static_cast<int>(dy);
        if (physY < 0) continue;
        if (static_cast<uint32_t>(physY) >= g_physFbHeight) break;

        uint32_t srcY = dy * scale;
        if (srcY >= srcH) break;
        const uint32_t* srcRow = src + srcY * srcStride;

        volatile uint32_t* dstRow = g_physFb + static_cast<uint32_t>(physY) * g_physFbStride;

        for (uint32_t dx = 0; dx < dstW; ++dx)
        {
            int physX = dstX0 + static_cast<int>(dx);
            if (physX < 0) continue;
            if (static_cast<uint32_t>(physX) >= g_physFbWidth) break;

            uint32_t srcX = dx * scale;
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

    for (uint32_t i = 0; i < g_compositedCount; ++i)
    {
        Process* p = g_compositedProcs[i];
        if (!p || p->state == ProcessState::Terminated)
            continue;
        if (!p->fbVirtual || p->fbVfbWidth == 0)
            continue;
        BlitProcess(p);
    }
}

} // namespace brook
