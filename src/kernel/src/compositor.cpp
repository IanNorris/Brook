#include "compositor.h"
#include "process.h"
#include "scheduler.h"
#include "tty.h"
#include "serial.h"
#include "memory/virtual_memory.h"
#include "memory/address.h"

namespace brook {

extern volatile uint64_t g_lapicTickCount;

// Cached physical framebuffer info.
static volatile uint32_t* g_physFb       = nullptr;
static uint32_t           g_physFbWidth  = 0;
static uint32_t           g_physFbHeight = 0;
static uint32_t           g_physFbStride = 0; // in pixels

// Registered process slots for compositing.
static constexpr uint32_t MAX_COMPOSITED = 64;
static Process* g_compositedProcs[MAX_COMPOSITED] = {};
static uint32_t g_compositedCount = 0;

// Compositor thread wakes every COMPOSITE_INTERVAL ms (~30 Hz).
static constexpr uint32_t COMPOSITE_INTERVAL = 33;

// Force-blit all processes every N ms regardless of dirty flag,
// so we see output even during init before DOOM signals dirty.
static constexpr uint32_t FORCE_BLIT_INTERVAL_MS = 500;
static uint32_t g_forceBlitCounter = 0;

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

volatile uint32_t* CompositorGetPhysFb(uint32_t* stride)
{
    if (stride) *stride = g_physFbStride;
    return g_physFb;
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
    proc->fbDirty       = 1;  // ensure first composite renders

    if (g_compositedCount < MAX_COMPOSITED)
        g_compositedProcs[g_compositedCount++] = proc;

    DbgPrintf("COMPOSITOR: proc '%s' pid %u → vfb %ux%u at 0x%lx, dest=(%d,%d) scale=%u\n",
                 proc->name, proc->pid, vfbWidth, vfbHeight, vfbAddr.raw(),
                 destX, destY, scale);
    return true;
}

// Blit a process's VFB onto the physical FB with optional downscaling.
// If forceAll is true, blit regardless of dirty flag (periodic refresh).
static void BlitProcess(Process* proc, bool forceAll)
{
    if (!proc->fbVirtual || proc->fbVfbWidth == 0) return;
    if (!forceAll && !proc->fbDirty) return;
    proc->fbDirty = 0;

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

    // Precompute clipping.
    uint32_t startDy = 0, startDx = 0;
    if (dstY0 < 0) startDy = static_cast<uint32_t>(-dstY0);
    if (dstX0 < 0) startDx = static_cast<uint32_t>(-dstX0);

    uint32_t endDy = dstH;
    uint32_t endDx = dstW;
    if (dstY0 + static_cast<int>(endDy) > static_cast<int>(g_physFbHeight))
        endDy = g_physFbHeight - static_cast<uint32_t>(dstY0);
    if (dstX0 + static_cast<int>(endDx) > static_cast<int>(g_physFbWidth))
        endDx = g_physFbWidth - static_cast<uint32_t>(dstX0);

    if (scale == 1)
    {
        // Fast path: memcpy entire rows.
        uint32_t copyWidth = (endDx - startDx) * 4;
        for (uint32_t dy = startDy; dy < endDy; ++dy)
        {
            const uint32_t* srcRow = src + dy * srcStride + startDx;
            volatile uint32_t* dstRow = g_physFb +
                static_cast<uint32_t>(dstY0 + dy) * g_physFbStride +
                static_cast<uint32_t>(dstX0 + startDx);
            __builtin_memcpy(const_cast<uint32_t*>(dstRow), srcRow, copyWidth);
        }
    }
    else
    {
        // Scaled path: sample every `scale` pixels.
        for (uint32_t dy = startDy; dy < endDy; ++dy)
        {
            uint32_t srcY = dy * scale;
            if (srcY >= srcH) break;
            const uint32_t* srcRow = src + srcY * srcStride;

            volatile uint32_t* dstRow = g_physFb +
                static_cast<uint32_t>(dstY0 + dy) * g_physFbStride;

            for (uint32_t dx = startDx; dx < endDx; ++dx)
            {
                uint32_t srcX = dx * scale;
                if (srcX >= srcW) break;
                dstRow[dstX0 + dx] = srcRow[srcX];
            }
        }
    }
}

// Global halt flag — set by panic to stop compositing.
static volatile bool g_compositorHalted = false;

static void CompositorLoop()
{
    if (!g_physFb || g_compositedCount == 0)
        return;

    // Every FORCE_BLIT_INTERVAL_MS, blit all processes regardless of dirty
    // flag. This ensures we see output during init (before DOOM signals dirty).
    g_forceBlitCounter += COMPOSITE_INTERVAL;
    bool forceAll = (g_forceBlitCounter >= FORCE_BLIT_INTERVAL_MS);
    if (forceAll) g_forceBlitCounter = 0;

    for (uint32_t i = 0; i < g_compositedCount; ++i)
    {
        Process* p = g_compositedProcs[i];
        if (!p) continue;

        // Handle exit color fill: paint the physical FB region directly
        // (no VFB read needed — safe even after VFB pages are freed).
        uint32_t exitColor = __atomic_load_n(&p->fbExitColor, __ATOMIC_ACQUIRE);
        if (exitColor)
        {
            uint32_t scale = p->fbScale ? p->fbScale : 1;
            uint32_t dstW = p->fbVfbWidth / scale;
            uint32_t dstH = p->fbVfbHeight / scale;
            int dstX0 = p->fbDestX;
            int dstY0 = p->fbDestY;

            for (uint32_t dy = 0; dy < dstH; ++dy)
            {
                int y = dstY0 + static_cast<int>(dy);
                if (y < 0 || static_cast<uint32_t>(y) >= g_physFbHeight) continue;
                volatile uint32_t* row = g_physFb + y * g_physFbStride;
                for (uint32_t dx = 0; dx < dstW; ++dx)
                {
                    int x = dstX0 + static_cast<int>(dx);
                    if (x < 0 || static_cast<uint32_t>(x) >= g_physFbWidth) continue;
                    row[x] = exitColor;
                }
            }
            __atomic_store_n(&p->fbExitColor, 0u, __ATOMIC_RELEASE);
            g_compositedProcs[i] = nullptr;
            continue;
        }

        if (p->state == ProcessState::Terminated)
        {
            g_compositedProcs[i] = nullptr;
            continue;
        }
        if (!p->fbVirtual || p->fbVfbWidth == 0)
            continue;

        BlitProcess(p, forceAll);
    }
}

static void CompositorThreadFn(void* /*arg*/)
{
    SerialPrintf("COMPOSITOR: thread started (%ux%u, %u processes)\n",
                 g_physFbWidth, g_physFbHeight, g_compositedCount);

    for (;;)
    {
        if (!g_compositorHalted)
            CompositorLoop();

        Process* self = ProcessCurrent();
        self->wakeupTick = g_lapicTickCount + COMPOSITE_INTERVAL;
        SchedulerBlock(self);
    }
}

void CompositorStartThread()
{
    Process* thread = KernelThreadCreate("compositor", CompositorThreadFn, nullptr);
    if (!thread)
    {
        SerialPuts("COMPOSITOR: failed to create thread\n");
        return;
    }
    SchedulerAddProcess(thread);
    SerialPrintf("COMPOSITOR: thread created pid=%u\n", thread->pid);
}

void CompositorHalt()
{
    g_compositorHalted = true;
}

void CompositorRemap(uint64_t fbPhys, uint32_t w, uint32_t h, uint32_t stridePixels)
{
    // The TTY has already mapped the new framebuffer. Get its virtual pointer.
    uint32_t* pixels;
    uint32_t tw, th, tStride;
    if (!TtyGetFramebuffer(&pixels, &tw, &th, &tStride)) {
        SerialPuts("COMPOSITOR: CompositorRemap — TtyGetFramebuffer failed\n");
        return;
    }

    g_physFb       = reinterpret_cast<volatile uint32_t*>(pixels);
    g_physFbWidth  = w;
    g_physFbHeight = h;
    g_physFbStride = stridePixels;

    SerialPrintf("COMPOSITOR: remapped to %ux%u stride=%u\n", w, h, stridePixels);
}

} // namespace brook
