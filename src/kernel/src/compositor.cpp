#include "compositor.h"
#include "process.h"
#include "scheduler.h"
#include "tty.h"
#include "serial.h"
#include "memory/virtual_memory.h"
#include "memory/address.h"
#include "mouse.h"
#include "window.h"
#include "terminal.h"
#include "input.h"
#include "net.h"
#include "font_atlas.h"
#include "debug_overlay.h"

namespace brook {

extern volatile uint64_t g_lapicTickCount;

// Cached physical framebuffer info.
static volatile uint32_t* g_physFb       = nullptr;
static uint32_t           g_physFbWidth  = 0;
static uint32_t           g_physFbHeight = 0;
static uint32_t           g_physFbStride = 0; // in pixels

// Double-buffer: compose into cached RAM, then bulk-copy to MMIO FB.
// MMIO writes are 10-100× slower than cached RAM; sequential bulk copy
// via memcpy is much faster than scattered pixel writes.
static uint32_t* g_backBuffer     = nullptr;
static uint32_t  g_backBufStride  = 0; // in pixels (matches physFb stride)

// Dirty-region tracking: only copy scanlines that changed to MMIO.
// At 1920×1080×4, a full flip is ~8MB of MMIO writes.  With dirty tracking
// we typically copy only the DOOM windows + cursor region (~1-2MB).
static uint32_t g_dirtyMinY = 0xFFFFFFFFu; // inclusive
static uint32_t g_dirtyMaxY = 0;          // exclusive

static inline void MarkDirtyRows(uint32_t minY, uint32_t maxY)
{
    if (minY < g_dirtyMinY) g_dirtyMinY = minY;
    if (maxY > g_dirtyMaxY) g_dirtyMaxY = maxY;
}

static inline void MarkAllDirty()
{
    g_dirtyMinY = 0;
    g_dirtyMaxY = g_physFbHeight;
}

// Registered process slots for compositing.
static constexpr uint32_t MAX_COMPOSITED = 64;
static Process* g_compositedProcs[MAX_COMPOSITED] = {};
static uint32_t g_compositedCount = 0;

// Frame epoch — incremented at the START of each compositor frame.
// ProcessDestroy uses this to wait until any in-progress blit has finished.
static volatile uint64_t g_compositorEpoch = 0;

// Global input "grabber" — set by waylandd via sys_brook_input_grab so it
// receives every keyboard/mouse event in addition to (or instead of) the
// kernel WM's per-window routing. NULL means no grabber registered.
static Process* g_inputGrabber = nullptr;

bool CompositorSetInputGrabber(Process* proc, bool enable)
{
    if (!proc) return false;
    if (enable) {
        __atomic_store_n(&g_inputGrabber, proc, __ATOMIC_RELEASE);
        SerialPrintf("COMPOSITOR: input grabber set pid=%u '%s'\n",
                     proc->pid, proc->name);
        return true;
    }
    Process* cur = __atomic_load_n(&g_inputGrabber, __ATOMIC_ACQUIRE);
    if (cur != proc) return false;
    __atomic_store_n(&g_inputGrabber, static_cast<Process*>(nullptr), __ATOMIC_RELEASE);
    SerialPrintf("COMPOSITOR: input grabber cleared (was pid=%u)\n", proc->pid);
    return true;
}

void CompositorClearInputGrabberIfMatches(Process* proc)
{
    if (!proc) return;
    Process* cur = __atomic_load_n(&g_inputGrabber, __ATOMIC_ACQUIRE);
    if (cur == proc) {
        __atomic_store_n(&g_inputGrabber, static_cast<Process*>(nullptr), __ATOMIC_RELEASE);
        SerialPrintf("COMPOSITOR: input grabber pid=%u exited, cleared\n", proc->pid);
    }
}

static inline void RouteToGrabber(const InputEvent& ev)
{
    Process* g = __atomic_load_n(&g_inputGrabber, __ATOMIC_ACQUIRE);
    if (g && g->state != ProcessState::Terminated)
        ProcessInputPush(g, ev);
}

// Compositor fallback interval: if no event-driven wakeup arrives within
// this many ms, the compositor wakes anyway (for cursor updates, etc.).
static constexpr uint32_t COMPOSITE_INTERVAL = 16; // ~60 Hz fallback

// Force-blit all processes every N ms regardless of dirty flag,
// so we see output even during init before DOOM signals dirty.
static constexpr uint32_t FORCE_BLIT_INTERVAL_MS = 500;
static uint64_t g_lastForceBlitTick = 0;

// Present-timing telemetry: ring buffer of recent frame periods (ms) and
// per-frame loop durations (ms).  Emitted on serial every 5s.  Additive;
// no rendering effect.  Useful for diagnosing invisible hitches that an
// app-level overlay (e.g. Q2's) wouldn't catch because they happen in the
// compositor itself or in scheduling between wakeups.
static constexpr uint32_t PRESENT_STATS_WINDOW = 256; // ~4s @ 60Hz
static uint32_t g_presentPeriodMs[PRESENT_STATS_WINDOW] = {};
static uint32_t g_presentLoopMs  [PRESENT_STATS_WINDOW] = {};
static uint32_t g_presentStatsIdx      = 0;
static uint32_t g_presentStatsFilled   = 0;
static uint64_t g_presentLastLoopTick  = 0;
static uint64_t g_presentLastReportTick= 0;

static void PresentStatsReport()
{
    uint64_t now = g_lapicTickCount;
    if (g_presentLastReportTick == 0) g_presentLastReportTick = now;
    if (now - g_presentLastReportTick < 50000) return;
    g_presentLastReportTick = now;

    uint32_t n = g_presentStatsFilled;
    if (n < 8) return; // need a real sample

    // Period: min, mean, p99, max.  Simple insertion-based threshold
    // counts (we don't sort the whole array to keep this cheap).
    uint64_t sumP = 0, sumL = 0;
    uint32_t minP = 0xFFFFFFFFu, maxP = 0, minL = 0xFFFFFFFFu, maxL = 0;
    // p99 estimate: count frames exceeding 33ms (missed 30Hz) and 50ms.
    uint32_t over33 = 0, over50 = 0, over100 = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t p = g_presentPeriodMs[i];
        uint32_t l = g_presentLoopMs[i];
        sumP += p; sumL += l;
        if (p < minP) minP = p; if (p > maxP) maxP = p;
        if (l < minL) minL = l; if (l > maxL) maxL = l;
        if (p >= 33)  ++over33;
        if (p >= 50)  ++over50;
        if (p >= 100) ++over100;
    }
    uint32_t meanP = static_cast<uint32_t>(sumP / n);
    uint32_t meanL = static_cast<uint32_t>(sumL / n);
    uint32_t fps   = meanP ? (1000u / meanP) : 0;

    SerialPrintf("COMPOSITOR stats[%u]: period min=%u mean=%u max=%u ms "
                 "(%u fps); loop min=%u mean=%u max=%u ms; "
                 "frames>33ms=%u >50ms=%u >100ms=%u\n",
                 n, minP, meanP, maxP, fps, minL, meanL, maxL,
                 over33, over50, over100);
}

// Wallpaper: raw XRGB pixel data loaded from disk, or solid color fallback.
static uint32_t* g_wallpaper       = nullptr;
static uint32_t  g_wallpaperWidth  = 0;
static uint32_t  g_wallpaperHeight = 0;

void CompositorInit()
{
    // Get the physical framebuffer address so we can map the real MMIO,
    // not the backbuffer that TtyGetFramebuffer() might return if a
    // display driver (bochs) has already called CompositorRemap.
    uint64_t fbPhysBase;
    uint32_t w, h, strideBytes;
    if (!TtyGetFramebufferPhys(&fbPhysBase, &w, &h, &strideBytes))
    {
        SerialPuts("COMPOSITOR: no framebuffer available\n");
        return;
    }

    g_physFb       = reinterpret_cast<volatile uint32_t*>(PhysToVirt(PhysicalAddress(fbPhysBase)).raw());
    g_physFbWidth  = w;
    g_physFbHeight = h;
    g_physFbStride = strideBytes / 4; // convert byte stride to pixel stride

    // Allocate a cached-RAM backbuffer for double-buffering.
    g_backBufStride = g_physFbStride;
    uint64_t bbSizeBytes = static_cast<uint64_t>(g_backBufStride) * h * 4;
    uint64_t bbPages = (bbSizeBytes + 4095) / 4096;
    VirtualAddress bbAddr = VmmAllocPages(bbPages, VMM_WRITABLE, MemTag::Device, 0);
    if (bbAddr)
    {
        g_backBuffer = reinterpret_cast<uint32_t*>(bbAddr.raw());
        // Copy current MMIO framebuffer content (boot logo, TTY text) into the
        // backbuffer so the compositor preserves existing screen content.
        for (uint32_t y = 0; y < h; ++y)
        {
            __builtin_memcpy(
                g_backBuffer + y * g_backBufStride,
                const_cast<uint32_t*>(g_physFb + y * g_physFbStride),
                w * 4);
        }
        SerialPrintf("COMPOSITOR: backbuffer %lu KB at 0x%lx\n",
                     bbSizeBytes / 1024, bbAddr.raw());

        // Redirect the TTY to render into the backbuffer instead of MMIO.
        // This ensures TTY text (clock, shell) appears in the compositor's
        // buffer and isn't overwritten on each frame flip.
        TtyRedirectToBackbuffer(g_backBuffer);
    }
    else
    {
        SerialPuts("COMPOSITOR: WARNING — backbuffer alloc failed, direct MMIO path\n");
    }

    // Set mouse cursor bounds to match screen resolution.
    MouseSetBounds(w, h);

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
    {
        uint32_t idx = g_compositedCount;
        __atomic_store_n(&g_compositedProcs[idx], proc, __ATOMIC_RELEASE);
        __atomic_store_n(&g_compositedCount, idx + 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&proc->compositorRegistered, true, __ATOMIC_RELEASE);

    DbgPrintf("COMPOSITOR: proc '%s' pid %u → vfb %ux%u at 0x%lx, dest=(%d,%d) scale=%u\n",
                 proc->name, proc->pid, vfbWidth, vfbHeight, vfbAddr.raw(),
                 destX, destY, scale);
    return true;
}

bool CompositorResizeVfb(Process* proc, uint32_t newWidth, uint32_t newHeight)
{
    if (!proc || !g_physFb) return false;
    if (newWidth == 0 || newHeight == 0) return false;

    // Clamp to physical FB so we can't allocate something that wouldn't fit.
    if (newWidth  > g_physFbWidth)  newWidth  = g_physFbWidth;
    if (newHeight > g_physFbHeight) newHeight = g_physFbHeight;

    if (proc->fbVfbWidth == newWidth && proc->fbVfbHeight == newHeight)
        return true;

    uint32_t stride = newWidth;
    uint64_t fbSizeBytes = static_cast<uint64_t>(stride) * newHeight * 4;
    uint64_t fbPages     = (fbSizeBytes + 4095) / 4096;

    VirtualAddress vfbAddr = VmmAllocPages(fbPages, VMM_WRITABLE, MemTag::Device, proc->pid);
    if (!vfbAddr)
    {
        SerialPrintf("COMPOSITOR: resize failed to alloc %lu pages for vfb %ux%u\n",
                     fbPages, newWidth, newHeight);
        return false;
    }

    auto* newVfb = reinterpret_cast<uint32_t*>(vfbAddr.raw());
    for (uint64_t i = 0; i < (fbSizeBytes / 4); ++i)
        newVfb[i] = 0;

    // Wait for any in-progress blit using the old VFB pointer to retire.
    CompositorWaitFrame();

    // Swap in the new VFB atomically; blits fetch fbVirtual atomically.
    __atomic_store_n(&proc->fbVirtual, newVfb, __ATOMIC_RELEASE);
    proc->fbVirtualSize = static_cast<uint32_t>(fbSizeBytes);
    proc->fbVfbWidth    = newWidth;
    proc->fbVfbHeight   = newHeight;
    proc->fbVfbStride   = stride;
    proc->fbDirty       = 1;

    // Old VFB pages are leaked until process exit (PmmKillPid reclaims them
    // via the pid-tagged page owner).  Acceptable for rare resize events.
    SerialPrintf("COMPOSITOR: proc '%s' pid %u vfb resized to %ux%u at 0x%lx\n",
                 proc->name, proc->pid, newWidth, newHeight, vfbAddr.raw());
    return true;
}

// Blit a process's VFB onto the physical FB with optional downscaling.
// If forceAll is true, blit regardless of dirty flag (periodic refresh).
static void BlitProcess(Process* proc, bool forceAll)
{
    const uint32_t* src = __atomic_load_n(&proc->fbVirtual, __ATOMIC_ACQUIRE);
    if (!src || proc->fbVfbWidth == 0) return;
    if (proc->state == ProcessState::Terminated) return;
    if (!forceAll && !proc->fbDirty) return;
    proc->fbDirty = 0;

    // One-time diagnostic: log first blit for each process
    static uint32_t s_blitLogMask = 0;
    uint32_t bit = (proc->pid < 32) ? (1u << proc->pid) : 0;
    if (bit && !(s_blitLogMask & bit)) {
        s_blitLogMask |= bit;
        SerialPrintf("COMPOSITOR: first blit pid %u '%s' vfb=%ux%u dest=(%d,%d) scale=%u fb=0x%lx\n",
                     proc->pid, proc->name, proc->fbVfbWidth, proc->fbVfbHeight,
                     proc->fbDestX, proc->fbDestY, proc->fbScale,
                     reinterpret_cast<uint64_t>(g_physFb));
    }

    // Use the atomically-snapshotted src pointer (don't re-read fbVirtual)
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

    // Track dirty scanlines for this blit.
    uint32_t blitMinY = static_cast<uint32_t>(dstY0) + startDy;
    uint32_t blitMaxY = static_cast<uint32_t>(dstY0) + endDy;
    MarkDirtyRows(blitMinY, blitMaxY);

    // Destination surface: backbuffer (fast cached RAM) or MMIO FB (slow).
    uint32_t*       dstBase   = g_backBuffer ? g_backBuffer : const_cast<uint32_t*>(g_physFb);
    const uint32_t  dstStride = g_backBuffer ? g_backBufStride : g_physFbStride;

    if (scale == 1)
    {
        // Fast path: memcpy entire rows.
        uint32_t copyWidth = (endDx - startDx) * 4;
        for (uint32_t dy = startDy; dy < endDy; ++dy)
        {
            const uint32_t* srcRow = src + dy * srcStride + startDx;
            uint32_t* dstRow = dstBase +
                static_cast<uint32_t>(dstY0 + dy) * dstStride +
                static_cast<uint32_t>(dstX0 + startDx);
            __builtin_memcpy(dstRow, srcRow, copyWidth);
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

            uint32_t* dstRow = dstBase +
                static_cast<uint32_t>(dstY0 + dy) * dstStride;

            for (uint32_t dx = startDx; dx < endDx; ++dx)
            {
                uint32_t srcX = dx * scale;
                if (srcX >= srcW) break;
                dstRow[dstX0 + dx] = srcRow[srcX];
            }
        }
    }
}

// Blit a process VFB at an explicit destination (for WM-managed windows).
// Supports integer upscaling via nearest-neighbor (for DOOM at 320×200 → 4×).
static void BlitProcessAt(Process* proc, int dstX0, int dstY0, bool forceAll,
                           uint8_t upscale = 1)
{
    // Snapshot VFB pointer atomically to avoid TOCTOU race with process exit
    // (another CPU may null fbVirtual between check and use).
    const uint32_t* src = __atomic_load_n(&proc->fbVirtual, __ATOMIC_ACQUIRE);
    if (!src || proc->fbVfbWidth == 0) return;
    if (proc->state == ProcessState::Terminated) return;
    if (!forceAll && !proc->fbDirty) return;
    proc->fbDirty = 0;

    const uint32_t  srcW = proc->fbVfbWidth;
    const uint32_t  srcH = proc->fbVfbHeight;
    const uint32_t  srcStride = proc->fbVfbStride;

    uint32_t*       dstBase   = g_backBuffer ? g_backBuffer : const_cast<uint32_t*>(g_physFb);
    const uint32_t  dstStride = g_backBuffer ? g_backBufStride : g_physFbStride;

    if (upscale <= 1)
    {
        // 1:1 blit (fast path)
        uint32_t startDy = 0, startDx = 0;
        if (dstY0 < 0) startDy = static_cast<uint32_t>(-dstY0);
        if (dstX0 < 0) startDx = static_cast<uint32_t>(-dstX0);

        uint32_t endDy = srcH;
        uint32_t endDx = srcW;
        if (dstY0 + static_cast<int>(endDy) > static_cast<int>(g_physFbHeight))
            endDy = g_physFbHeight - static_cast<uint32_t>(dstY0);
        if (dstX0 + static_cast<int>(endDx) > static_cast<int>(g_physFbWidth))
            endDx = g_physFbWidth - static_cast<uint32_t>(dstX0);

        MarkDirtyRows(static_cast<uint32_t>(dstY0) + startDy,
                      static_cast<uint32_t>(dstY0) + endDy);

        uint32_t copyWidth = (endDx - startDx) * 4;
        for (uint32_t dy = startDy; dy < endDy; ++dy)
        {
            const uint32_t* srcRow = src + dy * srcStride + startDx;
            uint32_t* dstRow = dstBase +
                static_cast<uint32_t>(dstY0 + dy) * dstStride +
                static_cast<uint32_t>(dstX0 + startDx);
            __builtin_memcpy(dstRow, srcRow, copyWidth);
        }
    }
    else
    {
        // Nearest-neighbor upscale
        uint32_t dispW = srcW * upscale;
        uint32_t dispH = srcH * upscale;

        uint32_t startDy = 0, startDx = 0;
        if (dstY0 < 0) startDy = static_cast<uint32_t>(-dstY0);
        if (dstX0 < 0) startDx = static_cast<uint32_t>(-dstX0);

        uint32_t endDy = dispH;
        uint32_t endDx = dispW;
        if (dstY0 + static_cast<int>(endDy) > static_cast<int>(g_physFbHeight))
            endDy = g_physFbHeight - static_cast<uint32_t>(dstY0);
        if (dstX0 + static_cast<int>(endDx) > static_cast<int>(g_physFbWidth))
            endDx = g_physFbWidth - static_cast<uint32_t>(dstX0);

        MarkDirtyRows(static_cast<uint32_t>(dstY0) + startDy,
                      static_cast<uint32_t>(dstY0) + endDy);

        for (uint32_t dy = startDy; dy < endDy; ++dy)
        {
            uint32_t srcY = dy / upscale;
            if (srcY >= srcH) break;
            const uint32_t* srcRow = src + srcY * srcStride;
            uint32_t* dstRow = dstBase +
                static_cast<uint32_t>(dstY0 + dy) * dstStride +
                static_cast<uint32_t>(dstX0);

            for (uint32_t dx = startDx; dx < endDx; ++dx)
            {
                uint32_t srcX = dx / upscale;
                if (srcX >= srcW) break;
                dstRow[dx] = srcRow[srcX];
            }
        }
    }
}

// Blit a kernel-resident VFB at an explicit destination.  Used for
// per-window VFBs (Window::vfb) — separate from the per-process
// `BlitProcessAt` so we don't need to fake a Process struct.
static void BlitWindowVfb(const uint32_t* src,
                           uint32_t srcW, uint32_t srcH, uint32_t srcStride,
                           int dstX0, int dstY0)
{
    if (!src || srcW == 0) return;
    uint32_t* dstBase = g_backBuffer ? g_backBuffer : const_cast<uint32_t*>(g_physFb);
    const uint32_t dstStride = g_backBuffer ? g_backBufStride : g_physFbStride;

    uint32_t startDy = 0, startDx = 0;
    if (dstY0 < 0) startDy = static_cast<uint32_t>(-dstY0);
    if (dstX0 < 0) startDx = static_cast<uint32_t>(-dstX0);

    uint32_t endDy = srcH;
    uint32_t endDx = srcW;
    if (dstY0 + static_cast<int>(endDy) > static_cast<int>(g_physFbHeight))
        endDy = g_physFbHeight - static_cast<uint32_t>(dstY0);
    if (dstX0 + static_cast<int>(endDx) > static_cast<int>(g_physFbWidth))
        endDx = g_physFbWidth - static_cast<uint32_t>(dstX0);

    if (endDy <= startDy || endDx <= startDx) return;

    MarkDirtyRows(static_cast<uint32_t>(dstY0) + startDy,
                  static_cast<uint32_t>(dstY0) + endDy);

    uint32_t copyWidth = (endDx - startDx) * 4;
    for (uint32_t dy = startDy; dy < endDy; ++dy)
    {
        const uint32_t* srcRow = src + dy * srcStride + startDx;
        uint32_t* dstRow = dstBase +
            static_cast<uint32_t>(dstY0 + dy) * dstStride +
            static_cast<uint32_t>(dstX0 + startDx);
        __builtin_memcpy(dstRow, srcRow, copyWidth);
    }
}

// Draw wallpaper (or solid background) into backbuffer.
static void BlitWallpaper()
{
    uint32_t*      dstBase   = g_backBuffer ? g_backBuffer : const_cast<uint32_t*>(g_physFb);
    const uint32_t dstStride = g_backBuffer ? g_backBufStride : g_physFbStride;

    if (g_wallpaper && g_wallpaperWidth > 0 && g_wallpaperHeight > 0)
    {
        uint32_t copyH = (g_wallpaperHeight < g_physFbHeight) ? g_wallpaperHeight : g_physFbHeight;
        uint32_t copyW = (g_wallpaperWidth < g_physFbWidth) ? g_wallpaperWidth : g_physFbWidth;
        uint32_t offX = (g_physFbWidth > copyW) ? (g_physFbWidth - copyW) / 2 : 0;
        uint32_t offY = (g_physFbHeight > copyH) ? (g_physFbHeight - copyH) / 2 : 0;

        // Clear edges if wallpaper doesn't fill
        if (offX > 0 || offY > 0)
        {
            for (uint32_t y = 0; y < g_physFbHeight; ++y)
                for (uint32_t x = 0; x < g_physFbWidth; ++x)
                    dstBase[y * dstStride + x] = 0x00001A3A;
        }

        for (uint32_t y = 0; y < copyH; ++y)
        {
            __builtin_memcpy(
                dstBase + (offY + y) * dstStride + offX,
                g_wallpaper + y * g_wallpaperWidth,
                copyW * 4);
        }
    }
    else
    {
        // No wallpaper loaded yet — preserve the current backbuffer content
        // (boot logo / previous TTY output) to avoid a black flash.
        // Once wallpaper is set via CompositorSetWallpaper, this branch
        // will stop being taken and the wallpaper fills the screen.
    }
    MarkAllDirty();
}

// ---------------------------------------------------------------------------
// Hardware cursor — simple 12×16 arrow pointer
// ---------------------------------------------------------------------------

// 1 = white, 2 = black outline, 0 = transparent
static const uint8_t g_cursorBitmap[16][12] = {
    {2,0,0,0,0,0,0,0,0,0,0,0},
    {2,2,0,0,0,0,0,0,0,0,0,0},
    {2,1,2,0,0,0,0,0,0,0,0,0},
    {2,1,1,2,0,0,0,0,0,0,0,0},
    {2,1,1,1,2,0,0,0,0,0,0,0},
    {2,1,1,1,1,2,0,0,0,0,0,0},
    {2,1,1,1,1,1,2,0,0,0,0,0},
    {2,1,1,1,1,1,1,2,0,0,0,0},
    {2,1,1,1,1,1,1,1,2,0,0,0},
    {2,1,1,1,1,1,1,1,1,2,0,0},
    {2,1,1,1,1,1,2,2,2,2,2,0},
    {2,1,1,2,1,1,2,0,0,0,0,0},
    {2,1,2,0,2,1,1,2,0,0,0,0},
    {2,2,0,0,2,1,1,2,0,0,0,0},
    {2,0,0,0,0,2,1,1,2,0,0,0},
    {0,0,0,0,0,2,2,2,2,0,0,0},
};

static constexpr uint32_t CURSOR_W = 12;
static constexpr uint32_t CURSOR_H = 16;

// Saved pixels under the cursor (for restore before re-blit)
static uint32_t g_cursorSave[CURSOR_W * CURSOR_H];
static int32_t  g_cursorSaveX = -1;
static int32_t  g_cursorSaveY = -1;
static bool     g_cursorVisible = false;

static void CursorSaveUnder(int32_t cx, int32_t cy)
{
    uint32_t* buf = g_backBuffer ? g_backBuffer : const_cast<uint32_t*>(g_physFb);
    uint32_t  stride = g_backBuffer ? g_backBufStride : g_physFbStride;
    for (uint32_t row = 0; row < CURSOR_H; row++)
    {
        int32_t sy = cy + static_cast<int32_t>(row);
        if (sy < 0 || static_cast<uint32_t>(sy) >= g_physFbHeight) continue;
        for (uint32_t col = 0; col < CURSOR_W; col++)
        {
            int32_t sx = cx + static_cast<int32_t>(col);
            if (sx < 0 || static_cast<uint32_t>(sx) >= g_physFbWidth) continue;
            g_cursorSave[row * CURSOR_W + col] = buf[sy * stride + sx];
        }
    }
    g_cursorSaveX = cx;
    g_cursorSaveY = cy;
    g_cursorVisible = true;
}

static void CursorRestore()
{
    if (!g_cursorVisible) return;
    uint32_t* buf = g_backBuffer ? g_backBuffer : const_cast<uint32_t*>(g_physFb);
    uint32_t  stride = g_backBuffer ? g_backBufStride : g_physFbStride;
    int32_t cx = g_cursorSaveX;
    int32_t cy = g_cursorSaveY;
    for (uint32_t row = 0; row < CURSOR_H; row++)
    {
        int32_t sy = cy + static_cast<int32_t>(row);
        if (sy < 0 || static_cast<uint32_t>(sy) >= g_physFbHeight) continue;
        for (uint32_t col = 0; col < CURSOR_W; col++)
        {
            int32_t sx = cx + static_cast<int32_t>(col);
            if (sx < 0 || static_cast<uint32_t>(sx) >= g_physFbWidth) continue;
            buf[sy * stride + sx] = g_cursorSave[row * CURSOR_W + col];
        }
    }
    g_cursorVisible = false;
}

static void CursorDraw(int32_t cx, int32_t cy)
{
    CursorSaveUnder(cx, cy);
    uint32_t* buf = g_backBuffer ? g_backBuffer : const_cast<uint32_t*>(g_physFb);
    uint32_t  stride = g_backBuffer ? g_backBufStride : g_physFbStride;
    for (uint32_t row = 0; row < CURSOR_H; row++)
    {
        int32_t sy = cy + static_cast<int32_t>(row);
        if (sy < 0 || static_cast<uint32_t>(sy) >= g_physFbHeight) continue;
        for (uint32_t col = 0; col < CURSOR_W; col++)
        {
            uint8_t px = g_cursorBitmap[row][col];
            if (px == 0) continue; // transparent
            int32_t sx = cx + static_cast<int32_t>(col);
            if (sx < 0 || static_cast<uint32_t>(sx) >= g_physFbWidth) continue;
            buf[sy * stride + sx] = (px == 1) ? 0x00FFFFFF : 0x00000000;
        }
    }
}

// Global halt flag — set by panic to stop compositing.
static volatile bool g_compositorHalted = false;

// The compositor Process pointer — set once the thread starts.
static Process* g_compositorProcess = nullptr;

// Forward declaration
static void CompositorHandleMouseWM();

// Button latch state — set by input queue event processing, consumed by mouse handler
static volatile bool g_wmBtnLatch        = false;
static volatile bool g_wmBtnReleaseLatch = false;

// WM-mode compositor loop: wallpaper → windows (z-ordered) → chrome → cursor.
static void CompositorLoopWM()
{
    uint64_t now = g_lapicTickCount;
    bool forceAll = (now - g_lastForceBlitTick >= FORCE_BLIT_INTERVAL_MS);
    if (forceAll) g_lastForceBlitTick = now;

    // 1. Draw wallpaper as background
    BlitWallpaper();

    // 2. Get windows in z-order (back to front) and blit their VFBs
    int sorted[WM_MAX_WINDOWS];
    uint32_t wcount = WmGetZOrder(sorted, WM_MAX_WINDOWS);

    for (uint32_t i = 0; i < wcount; ++i)
    {
        Window* w = WmGetWindow(sorted[i]);
        if (!w || !w->proc || !w->visible || w->minimized) continue;

        Process* p = w->proc;

        // Handle terminated processes
        if (p->state == ProcessState::Terminated)
        {
            // If this is a terminal window, shut down the terminal properly
            Terminal* term = TerminalFindByProcess(p);
            if (term)
                TerminalClose(term);

            // Destroy every window owned by this process in one go,
            // *before* unregistering / marking reapable.  Otherwise the
            // next loop iteration that dereferences w->proc on a sibling
            // window of the same proc may hit a freed Process struct
            // (the reaper can race us between iterations).
            WmDestroyWindowForProcess(p);
            CompositorUnregisterProcess(p);
            continue;
        }

        // Update process destX/destY to match window client area
        p->fbDestX = w->clientX();
        p->fbDestY = w->clientY();

        // Blit content then chrome per-window so z-order is respected.
        // (Previously chrome was drawn after ALL content, so a background
        // window's border could overwrite a foreground window's content.)
        if (w->vfb)
        {
            // Per-window VFB (Phase A): kernel-resident buffer owned by
            // the Window itself.  We always blit (the backbuffer is
            // wiped to wallpaper each frame, so skipping based on dirty
            // makes the content flicker out).  vfbDirty is consumed only
            // to clear the damage-bit; later phases can use it to skip
            // recomposition when nothing has changed *anywhere*.
            //
            // Clamp blit to the buffer that was actually allocated — the
            // user can drag-resize the window larger than the VFB before
            // the client provides a fresh buffer, and reading past
            // w->vfb causes a kernel #PF.
            uint32_t vfbW = w->vfbStride;
            uint32_t vfbH = (w->vfbStride && w->vfbBytes)
                          ? static_cast<uint32_t>(w->vfbBytes / (uint64_t)w->vfbStride / 4)
                          : 0;
            uint32_t blitW = w->clientW < vfbW ? w->clientW : vfbW;
            uint32_t blitH = w->clientH < vfbH ? w->clientH : vfbH;
            if (blitW && blitH)
                BlitWindowVfb(w->vfb, blitW, blitH, w->vfbStride,
                              w->clientX(), w->clientY());
            w->vfbDirty = 0;
        }
        else if (p->state != ProcessState::Terminated && p->fbVfbWidth > 0)
        {
            BlitProcessAt(p, w->clientX(), w->clientY(), true, w->upscale);
        }

        // Draw text cursor for terminal windows
        if (g_backBuffer)
        {
            Terminal* term = TerminalFindByProcess(p);
            if (term && term->active)
            {
                // Blink: visible for 500ms, hidden for 500ms
                bool cursorOn = ((now / 500) & 1) == 0;
                if (cursorOn)
                {
                    uint32_t lineH = static_cast<uint32_t>(g_fontAtlas.lineHeight);
                    uint32_t glyphW = g_fontAtlas.glyphCount > 0
                                      ? static_cast<uint32_t>(g_fontAtlas.glyphs[0].advance) : 8;
                    uint32_t cx = term->curX * glyphW;
                    uint32_t cy = term->curY * lineH;
                    uint8_t  sc = w->upscale;
                    int      ox = w->clientX();
                    int      oy = w->clientY();

                    for (uint32_t dy = 0; dy < lineH; dy++)
                    {
                        for (uint32_t dx = 0; dx < glyphW; dx++)
                        {
                            for (uint8_t sy = 0; sy < sc; sy++)
                            {
                                for (uint8_t sx = 0; sx < sc; sx++)
                                {
                                    int px = ox + static_cast<int>((cx + dx) * sc + sx);
                                    int py = oy + static_cast<int>((cy + dy) * sc + sy);
                                    if (px >= 0 && px < static_cast<int>(g_physFbWidth) &&
                                        py >= 0 && py < static_cast<int>(g_physFbHeight))
                                    {
                                        uint32_t& pixel = g_backBuffer[py * g_backBufStride + px];
                                        pixel ^= 0x00FFFFFF; // invert RGB
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (g_backBuffer)
            WmRenderChromeForWindow(g_backBuffer, g_backBufStride,
                                     g_physFbWidth, g_physFbHeight, sorted[i]);
    }

    // 3. Render taskbar at bottom of screen
    if (g_backBuffer)
    {
        WmRenderTaskbar(g_backBuffer, g_backBufStride,
                        g_physFbWidth, g_physFbHeight, now);
        uint32_t tbY = g_physFbHeight - WM_TASKBAR_HEIGHT;
        MarkDirtyRows(tbY, g_physFbHeight);

        // Render launcher popup over everything if open
        if (WmLauncherVisible())
        {
            WmLauncherRender(g_backBuffer, g_backBufStride,
                             g_physFbWidth, g_physFbHeight);
            // Mark the launcher area dirty (it's above the taskbar)
            MarkDirtyRows(0, g_physFbHeight);
        }
    }

    // 4. Route input events (keyboard + mouse buttons)
    // Must happen before mouse click handling so poll drains the virtqueue
    // and events are latched for CompositorHandleMouseWM.
    InputEvent ev;
    while (InputPollEvent(&ev))
    {
        // Always copy the event to the global grabber (e.g. waylandd) so
        // userspace display servers see every input regardless of WM
        // hit-test result.
        RouteToGrabber(ev);

        if (ev.type == InputEventType::MouseButtonDown && ev.scanCode == 0)
        {
            g_wmBtnLatch = true;
            // Also route LMB to the window under the cursor so Wayland
            // apps can see it.  WM drag/resize uses the latch above; this
            // forwards to the app's input queue if the click lands inside
            // a client area.
            int32_t mx = 0, my = 0;
            MouseGetPosition(&mx, &my);
            WmHitResult hit = WmHitTest(mx, my);
            if (hit.windowIndex >= 0 && hit.zone == WmHitZone::ClientArea)
            {
                Window* target = WmGetWindow(hit.windowIndex);
                if (target && target->proc)
                {
                    ProcessInputPush(target->proc, ev);
                    WmInputPush(target, ev,
                                static_cast<int16_t>(mx - target->clientX()),
                                static_cast<int16_t>(my - target->clientY()));
                }
            }
            continue;
        }
        if (ev.type == InputEventType::MouseButtonUp && ev.scanCode == 0)
        {
            g_wmBtnReleaseLatch = true;
            int32_t mx = 0, my = 0;
            MouseGetPosition(&mx, &my);
            WmHitResult hit = WmHitTest(mx, my);
            if (hit.windowIndex >= 0 && hit.zone == WmHitZone::ClientArea)
            {
                Window* target = WmGetWindow(hit.windowIndex);
                if (target && target->proc)
                {
                    ProcessInputPush(target->proc, ev);
                    WmInputPush(target, ev,
                                static_cast<int16_t>(mx - target->clientX()),
                                static_cast<int16_t>(my - target->clientY()));
                }
            }
            continue;
        }
        // Non-LMB mouse buttons (middle, right, side): route to window under cursor
        if (ev.type == InputEventType::MouseButtonDown ||
            ev.type == InputEventType::MouseButtonUp)
        {
            int32_t mx = 0, my = 0;
            MouseGetPosition(&mx, &my);
            WmHitResult hit = WmHitTest(mx, my);
            if (hit.windowIndex >= 0 && hit.zone == WmHitZone::ClientArea)
            {
                Window* target = WmGetWindow(hit.windowIndex);
                if (target && target->proc)
                {
                    ProcessInputPush(target->proc, ev);
                    WmInputPush(target, ev,
                                static_cast<int16_t>(mx - target->clientX()),
                                static_cast<int16_t>(my - target->clientY()));
                }
            }
            continue;
        }
        if (ev.type == InputEventType::MouseMove)
        {
            // Route motion to the window under the cursor (no latching needed
            // since the WM uses MouseGetPosition directly for drag/resize).
            int32_t mx = 0, my = 0;
            MouseGetPosition(&mx, &my);
            WmHitResult hit = WmHitTest(mx, my);
            if (hit.windowIndex >= 0 && hit.zone == WmHitZone::ClientArea)
            {
                Window* target = WmGetWindow(hit.windowIndex);
                if (target && target->proc)
                {
                    ProcessInputPush(target->proc, ev);
                    WmInputPush(target, ev,
                                static_cast<int16_t>(mx - target->clientX()),
                                static_cast<int16_t>(my - target->clientY()));
                }
            }
            continue;
        }

        // Scroll wheel: route to the window under the cursor (not the focused
        // one — standard UX is "scroll the thing you're pointing at").
        if (ev.type == InputEventType::MouseScroll)
        {
            int32_t mx = 0, my = 0;
            MouseGetPosition(&mx, &my);
            WmHitResult hit = WmHitTest(mx, my);
            if (hit.windowIndex >= 0)
            {
                Window* target = WmGetWindow(hit.windowIndex);
                if (target && target->proc)
                {
                    int8_t dy = static_cast<int8_t>(ev.scanCode);
                    int8_t dx = static_cast<int8_t>(ev.ascii);
                    // If the target is a terminal, consume the scroll as
                    // scrollback navigation.  Otherwise drop for now — we
                    // don't have a per-window scroll event channel yet.
                    Terminal* term = TerminalFindByProcess(target->proc);
                    if (term)
                    {
                        TerminalScroll(term, dy);
                        (void)dx;
                    }
                }
            }
            continue;
        }

        if (ev.type != InputEventType::KeyPress && ev.type != InputEventType::KeyRelease)
            continue;

        // Find the focused window (needed for both press and release)
        Window* focused = nullptr;
        for (uint32_t i = 0; i < WM_MAX_WINDOWS; i++)
        {
            Window* w = WmGetWindow(static_cast<int>(i));
            if (w && w->proc && w->focused && w->visible)
            {
                focused = w;
                break;
            }
        }
        if (!focused) continue;

        // Key release events skip hotkey/signal processing — just route to window
        if (ev.type == InputEventType::KeyRelease)
        {
            // Route release to terminal or per-process queue (same logic as press)
        }
        else
        {
        DbgPrintf("KEY: scan=0x%02x ascii=0x%02x\n", ev.scanCode, ev.ascii);

        // Global hotkeys (no focused window needed)
        if (ev.modifiers & INPUT_MOD_CTRL)
        {
            // Ctrl+T — spawn new terminal window
            if (ev.scanCode == 0x14) // T
            {
                WmSpawnTerminal();
                continue;
            }
        }

        // Check for terminal signal keys (Ctrl+C, Ctrl+Z, Ctrl+\)
        if (ev.modifiers & INPUT_MOD_CTRL)
        {
            int signum = 0;
            if (ev.scanCode == 0x2E) signum = 2;   // Ctrl+C → SIGINT
            else if (ev.scanCode == 0x2C) signum = 20; // Ctrl+Z → SIGTSTP
            else if (ev.scanCode == 0x2B) signum = 3;  // Ctrl+\ → SIGQUIT

            if (signum)
            {
                // Find the terminal's child process and send signal to its group
                for (uint32_t ti = 0; ti < MAX_TERMINALS; ti++)
                {
                    Terminal* t = TerminalGet(static_cast<int>(ti));
                    if (t && t->child == focused->proc)
                    {
                        // Echo control character to terminal
                        if (signum == 2) // Ctrl+C → ^C
                        {
                            char ctrlc = 0x03;
                            TerminalWriteInput(static_cast<int>(ti), &ctrlc, 1);
                        }
                        else if (signum == 20) // Ctrl+Z → ^Z
                        {
                            char ctrlz = 0x1A;
                            TerminalWriteInput(static_cast<int>(ti), &ctrlz, 1);
                        }
                        // Send signal to foreground process group
                        ProcessSendSignalToGroup(t->foregroundPgid, signum);
                        break;
                    }
                }
                continue;
            }
        }
        } // end KeyPress-only block

        // Route to terminal if this window's process has one
        bool routed = false;
        for (uint32_t ti = 0; ti < MAX_TERMINALS; ti++)
        {
            Terminal* t = TerminalGet(static_cast<int>(ti));
            if (!t) continue;
            if (t->child == focused->proc)
            {
                if (ev.ascii != 0)
                {
                    char ch = ev.ascii;
                    TerminalWriteInput(static_cast<int>(ti), &ch, 1);
                }
                else if (ev.type == InputEventType::KeyPress)
                {
                    // Convert extended scancodes to ANSI escape sequences
                    const char* seq = nullptr;
                    switch (ev.scanCode)
                    {
                    case SC_EXT_UP:     seq = "\x1b[A"; break;
                    case SC_EXT_DOWN:   seq = "\x1b[B"; break;
                    case SC_EXT_RIGHT:  seq = "\x1b[C"; break;
                    case SC_EXT_LEFT:   seq = "\x1b[D"; break;
                    case SC_EXT_HOME:   seq = "\x1b[H"; break;
                    case SC_EXT_END:    seq = "\x1b[F"; break;
                    case SC_EXT_INSERT: seq = "\x1b[2~"; break;
                    case SC_EXT_DELETE: seq = "\x1b[3~"; break;
                    case SC_EXT_PGUP:   seq = "\x1b[5~"; break;
                    case SC_EXT_PGDN:   seq = "\x1b[6~"; break;
                    }
                    if (seq)
                    {
                        uint32_t slen = 0;
                        while (seq[slen]) slen++;
                        TerminalWriteInput(static_cast<int>(ti), seq, slen);
                    }
                }
                routed = true;
                break;
            }
        }

        // Non-terminal window: push raw event to per-process input queue
        if (!routed)
            ProcessInputPush(focused->proc, ev);

        // Always also push to the per-window queue (key events, x/y zero).
        // Wayland clients rely on this for keyboard delivery.
        WmInputPush(focused, ev, 0, 0);
    }

    // 5. Handle mouse interaction (position + latched clicks)
    // Must run AFTER event poll so latch flags are populated.
    CompositorHandleMouseWM();
}

// Mouse state for WM interaction
static bool    g_wmDragging      = false;
static int     g_wmDragWindow    = -1;
static int16_t g_wmDragOffsetX   = 0;
static int16_t g_wmDragOffsetY   = 0;
static bool    g_wmResizing      = false;
static int     g_wmResizeWindow  = -1;
static int16_t g_wmResizeStartMX = 0;
static int16_t g_wmResizeStartMY = 0;
static uint16_t g_wmResizeStartW = 0;
static uint16_t g_wmResizeStartH = 0;
static bool    g_wmResizeX       = false; // resize horizontally
static bool    g_wmResizeY       = false; // resize vertically
static bool    g_wmLastBtnDown   = false;

static void CompositorHandleMouseWM()
{
    if (!MouseIsAvailable()) return;

    int32_t mx, my;
    MouseGetPosition(&mx, &my);

    // Determine button state. Use latched press/release from input queue
    // to survive fast press+release cycles between compositor frames.
    // Also check polled state for held buttons (drag operations).
    uint8_t polledButtons = MouseGetButtons();
    bool btnDown = (polledButtons & 0x01) != 0;

    // If a press was latched, treat as button down regardless of current state
    if (g_wmBtnLatch)
    {
        btnDown = true;
        g_wmBtnLatch = false;
    }

    if (btnDown && !g_wmLastBtnDown)
    {
        DbgPrintf("WM: click at (%d,%d)\n", mx, my);
        // If launcher is open, check launcher panel first
        if (WmLauncherVisible())
        {
            int launcherIdx = WmLauncherHitTest(mx, my, g_physFbWidth, g_physFbHeight);
            if (launcherIdx >= 0)
            {
                WmLauncherExec(launcherIdx);
                g_wmLastBtnDown = btnDown;
                return;
            }
            // Check if clicking the Apps button again (toggle off)
            int tbIdx = WmTaskbarHitTest(mx, my, g_physFbWidth, g_physFbHeight);
            if (tbIdx == -3)
            {
                WmLauncherToggle();
                g_wmLastBtnDown = btnDown;
                return;
            }
            // Clicked elsewhere — close launcher
            WmLauncherToggle();
            g_wmLastBtnDown = btnDown;
            return;
        }

        // Check taskbar first
        int tbIdx = WmTaskbarHitTest(mx, my, g_physFbWidth, g_physFbHeight);
        if (tbIdx == -3)
        {
            // "Apps" button — toggle launcher
            WmLauncherToggle();
        }
        else if (tbIdx == -2)
        {
            // "+" button — spawn new terminal
            WmSpawnTerminal();
        }
        else if (tbIdx >= 0)
        {
            Window* tw = WmGetWindow(tbIdx);
            if (tw && tw->proc)
            {
                if (tw->minimized)
                {
                    WmRestoreWindow(tbIdx);
                }
                else if (tw->focused)
                {
                    WmMinimizeWindow(tbIdx);
                }
                else
                {
                    WmSetFocus(tbIdx);
                }
            }
        }
        else if (my >= static_cast<int32_t>(g_physFbHeight - WM_TASKBAR_HEIGHT))
        {
            // Clicked taskbar background — do nothing
        }
        else
        {
        // Button just pressed — do hit test
        WmHitResult hit = WmHitTest(mx, my);

        if (hit.windowIndex >= 0)
        {
            WmSetFocus(hit.windowIndex);

            switch (hit.zone)
            {
            case WmHitZone::CloseButton:
            {
                Window* w = WmGetWindow(hit.windowIndex);
                if (w && w->proc)
                {
                    SerialPrintf("WM: close window %d '%s'\n", hit.windowIndex, w->title);
                    if (w->vfb)
                    {
                        // WM-API window: push CloseRequested so the client
                        // can do its own teardown (close wl_clients, free
                        // resources, then call WM_DESTROY_WINDOW).
                        WmPushWmEvent(w, WM_EVT_CLOSE_REQUESTED, 0, 0);
                    }
                    else
                    {
                        // Native/legacy window: previous behaviour.
                        Terminal* term = TerminalFindByProcess(w->proc);
                        if (term)
                            TerminalClose(term);
                        else
                            ProcessSendSignal(w->proc, 15); // SIGTERM
                    }
                }
                break;
            }
            case WmHitZone::MaximizeButton:
                WmToggleMaximize(hit.windowIndex);
                break;
            case WmHitZone::MinimizeButton:
                WmMinimizeWindow(hit.windowIndex);
                break;
            case WmHitZone::TitleBar:
            {
                // Start drag
                Window* w = WmGetWindow(hit.windowIndex);
                if (w && w->state != WindowState::Maximized)
                {
                    g_wmDragging = true;
                    g_wmDragWindow = hit.windowIndex;
                    g_wmDragOffsetX = static_cast<int16_t>(mx - w->x);
                    g_wmDragOffsetY = static_cast<int16_t>(my - w->y);
                }
                break;
            }
            case WmHitZone::ResizeCorner:
            case WmHitZone::ResizeRight:
            case WmHitZone::ResizeBottom:
            {
                // Start resize drag
                Window* w = WmGetWindow(hit.windowIndex);
                if (w && w->state != WindowState::Maximized)
                {
                    g_wmResizing = true;
                    g_wmResizeWindow = hit.windowIndex;
                    g_wmResizeStartMX = static_cast<int16_t>(mx);
                    g_wmResizeStartMY = static_cast<int16_t>(my);
                    g_wmResizeStartW  = w->clientW;
                    g_wmResizeStartH  = w->clientH;
                    g_wmResizeX = (hit.zone != WmHitZone::ResizeBottom);
                    g_wmResizeY = (hit.zone != WmHitZone::ResizeRight);
                }
                break;
            }
            default:
                break;
            }
        }
        } // end else (desktop hit test)
    }
    else if (btnDown && g_wmDragging)
    {
        // Continue drag
        WmMoveWindow(g_wmDragWindow,
                     static_cast<int16_t>(mx - g_wmDragOffsetX),
                     static_cast<int16_t>(my - g_wmDragOffsetY));
    }
    else if (btnDown && g_wmResizing)
    {
        // During resize drag, only update window dimensions (no VFB realloc).
        // The actual VFB reallocation happens on release.
        int16_t dx = static_cast<int16_t>(mx) - g_wmResizeStartMX;
        int16_t dy = static_cast<int16_t>(my) - g_wmResizeStartMY;
        int32_t newW = static_cast<int32_t>(g_wmResizeStartW) + (g_wmResizeX ? dx : 0);
        int32_t newH = static_cast<int32_t>(g_wmResizeStartH) + (g_wmResizeY ? dy : 0);
        if (newW < static_cast<int32_t>(WM_MIN_WIDTH))  newW = WM_MIN_WIDTH;
        if (newH < static_cast<int32_t>(WM_MIN_HEIGHT)) newH = WM_MIN_HEIGHT;

        // Update outer dimensions only — compositor will clip blit to VFB size
        Window* w = WmGetWindow(g_wmResizeWindow);
        if (w)
        {
            w->clientW = static_cast<uint16_t>(newW);
            w->clientH = static_cast<uint16_t>(newH);
        }
    }
    else if (!btnDown)
    {
        // On release, do the actual VFB resize if we were resizing
        if (g_wmResizing && g_wmResizeWindow >= 0)
        {
            Window* w = WmGetWindow(g_wmResizeWindow);
            if (w && w->proc)
            {
                WmResizeWindow(g_wmResizeWindow, w->clientW, w->clientH);
            }
        }
        g_wmDragging = false;
        g_wmDragWindow = -1;
        g_wmResizing = false;
        g_wmResizeWindow = -1;
    }

    g_wmLastBtnDown = btnDown;
}

// ---------------------------------------------------------------------------
// Clock overlay — rendered directly by the compositor each frame
// ---------------------------------------------------------------------------

static char* FormatUptime(char* buf, uint64_t totalSec)
{
    uint32_t h = static_cast<uint32_t>(totalSec / 3600);
    uint32_t m = static_cast<uint32_t>((totalSec % 3600) / 60);
    uint32_t s = static_cast<uint32_t>(totalSec % 60);

    buf[0] = static_cast<char>('0' + (h / 10) % 10);
    buf[1] = static_cast<char>('0' + h % 10);
    buf[2] = ':';
    buf[3] = static_cast<char>('0' + m / 10);
    buf[4] = static_cast<char>('0' + m % 10);
    buf[5] = ':';
    buf[6] = static_cast<char>('0' + s / 10);
    buf[7] = static_cast<char>('0' + s % 10);
    buf[8] = '\0';
    return buf;
}

static uint32_t ClockMeasureText(const char* text)
{
    const FontAtlas& fa = g_fontAtlas;
    uint32_t w = 0;
    while (*text)
    {
        int code = static_cast<int>(static_cast<uint8_t>(*text));
        if (code >= static_cast<int>(fa.firstChar) &&
            code < static_cast<int>(fa.firstChar + fa.glyphCount))
            w += static_cast<uint32_t>(fa.glyphs[code - static_cast<int>(fa.firstChar)].advance);
        ++text;
    }
    return w;
}

static void ClockDrawText(uint32_t* fb, uint32_t fbStride,
                           uint32_t fbW, uint32_t fbH,
                           const char* text, uint32_t px, uint32_t py,
                           uint32_t colour)
{
    const FontAtlas& fa = g_fontAtlas;
    while (*text)
    {
        int code = static_cast<int>(static_cast<uint8_t>(*text));
        if (code >= static_cast<int>(fa.firstChar) &&
            code < static_cast<int>(fa.firstChar + fa.glyphCount))
        {
            const GlyphInfo& gi = fa.glyphs[code - static_cast<int>(fa.firstChar)];
            int gw = gi.atlasX1 - gi.atlasX0;
            int gh = gi.atlasY1 - gi.atlasY0;
            int drawX = static_cast<int>(px) + gi.bearingX;
            int drawY = static_cast<int>(py) + fa.ascent - gi.bearingY;

            for (int row = 0; row < gh; ++row)
            {
                int absY = drawY + row;
                if (absY < 0 || static_cast<uint32_t>(absY) >= fbH) continue;
                for (int col = 0; col < gw; ++col)
                {
                    int absX = drawX + col;
                    if (absX < 0 || static_cast<uint32_t>(absX) >= fbW) continue;
                    uint8_t cov = fa.pixels[(gi.atlasY0 + row) *
                        static_cast<int>(fa.atlasWidth) + (gi.atlasX0 + col)];
                    if (cov > 0)
                    {
                        uint32_t r = ((colour >> 16) & 0xFF) * cov / 255;
                        uint32_t g = ((colour >> 8) & 0xFF) * cov / 255;
                        uint32_t b = (colour & 0xFF) * cov / 255;
                        fb[absY * fbStride + absX] = (r << 16) | (g << 8) | b;
                    }
                }
            }
            px += static_cast<uint32_t>(gi.advance);
        }
        ++text;
    }
}

// Draw the uptime clock in the top-right corner of the backbuffer.
static void CompositorDrawClock()
{
    uint32_t* dst = g_backBuffer ? g_backBuffer : const_cast<uint32_t*>(g_physFb);
    uint32_t stride = g_backBuffer ? g_backBufStride : g_physFbStride;
    if (!dst) return;

    uint64_t uptimeSec = g_lapicTickCount / 1000;
    char buf[16];
    FormatUptime(buf, uptimeSec);

    constexpr uint32_t padding = 4;
    constexpr uint32_t colour = 0x00FF00; // green
    uint32_t lineH = static_cast<uint32_t>(g_fontAtlas.lineHeight);
    uint32_t textW = ClockMeasureText(buf);
    uint32_t x = g_physFbWidth - textW - padding;
    uint32_t y = padding;

    // Clear background rect
    for (uint32_t row = y; row < y + lineH + 2 && row < g_physFbHeight; ++row)
        for (uint32_t col = x > 2 ? x - 2 : 0; col < g_physFbWidth; ++col)
            dst[row * stride + col] = 0x000000;

    ClockDrawText(dst, stride, g_physFbWidth, g_physFbHeight, buf, x, y, colour);
    MarkDirtyRows(y, y + lineH + 2);
}

static void CompositorLoop()
{
    if (!g_physFb)
        return;

    // Present-timing: sample period (wakeup-to-wakeup) and mark loop start.
    uint64_t loopStartTick = g_lapicTickCount;
    uint64_t periodMs = g_presentLastLoopTick
        ? (loopStartTick - g_presentLastLoopTick) : 0;
    g_presentLastLoopTick = loopStartTick;

    // Bump frame epoch so ProcessDestroy can wait for in-progress blits.
    __atomic_add_fetch(&g_compositorEpoch, 1, __ATOMIC_ACQ_REL);

    // Restore pixels under the old cursor position before blitting.
    if (g_cursorVisible) {
        uint32_t oldMinY = (g_cursorSaveY >= 0) ? static_cast<uint32_t>(g_cursorSaveY) : 0;
        uint32_t oldMaxY = static_cast<uint32_t>(g_cursorSaveY) + CURSOR_H;
        if (oldMaxY > g_physFbHeight) oldMaxY = g_physFbHeight;
        CursorRestore();
        MarkDirtyRows(oldMinY, oldMaxY);
    }

    // Branch: WM mode vs legacy mode
    if (WmIsActive())
    {
        CompositorLoopWM();
    }
    else
    {
    // --- Legacy compositor path (non-WM) ---
    uint32_t compCount = __atomic_load_n(&g_compositedCount, __ATOMIC_ACQUIRE);
    if (compCount > 0)
    {
    uint64_t now = g_lapicTickCount;
    bool forceAll = (now - g_lastForceBlitTick >= FORCE_BLIT_INTERVAL_MS);
    if (forceAll) g_lastForceBlitTick = now;

    for (uint32_t i = 0; i < compCount; ++i)
    {
        Process* p = __atomic_load_n(&g_compositedProcs[i], __ATOMIC_ACQUIRE);
        if (!p) continue;

        if (p->state == ProcessState::Terminated)
        {
            uint32_t exitColor = __atomic_load_n(&p->fbExitColor, __ATOMIC_ACQUIRE);
            if (exitColor)
            {
                uint32_t* dstBase = g_backBuffer ? g_backBuffer : const_cast<uint32_t*>(g_physFb);
                uint32_t  dstStride = g_backBuffer ? g_backBufStride : g_physFbStride;
                uint32_t scale = p->fbScale ? p->fbScale : 1;
                uint32_t dstW = p->fbVfbWidth / scale;
                uint32_t dstH = p->fbVfbHeight / scale;
                int dstX0 = p->fbDestX;
                int dstY0 = p->fbDestY;

                for (uint32_t dy = 0; dy < dstH; ++dy)
                {
                    int y = dstY0 + static_cast<int>(dy);
                    if (y < 0 || static_cast<uint32_t>(y) >= g_physFbHeight) continue;
                    uint32_t* row = dstBase + y * dstStride;
                    for (uint32_t dx = 0; dx < dstW; ++dx)
                    {
                        int x = dstX0 + static_cast<int>(dx);
                        if (x < 0 || static_cast<uint32_t>(x) >= g_physFbWidth) continue;
                        row[x] = exitColor;
                    }
                }
                __atomic_store_n(&p->fbExitColor, 0u, __ATOMIC_RELEASE);
                uint32_t minY = (dstY0 >= 0) ? static_cast<uint32_t>(dstY0) : 0;
                uint32_t maxY = static_cast<uint32_t>(dstY0) + dstH;
                if (maxY > g_physFbHeight) maxY = g_physFbHeight;
                MarkDirtyRows(minY, maxY);
            }
            __atomic_store_n(&g_compositedProcs[i], static_cast<Process*>(nullptr), __ATOMIC_RELEASE);
            __atomic_store_n(&p->compositorRegistered, false, __ATOMIC_RELEASE);
            if (p->state == ProcessState::Terminated)
                __atomic_store_n(&p->reapable, true, __ATOMIC_RELEASE);
            continue;
        }
        if (!p->fbVirtual || p->fbVfbWidth == 0)
            continue;

        BlitProcess(p, forceAll);
    }
    } // end if (g_compositedCount > 0)
    // Even with no composited windows, drain input events so the grabber
    // (waylandd) can still see them. In WM mode this drain happens inside
    // CompositorLoopWM; in legacy mode we need our own.
    {
        InputEvent ev;
        while (InputPollEvent(&ev))
            RouteToGrabber(ev);
    }
    } // end legacy mode

    // Draw mouse cursor on top of everything.
    if (MouseIsAvailable())
    {
        int32_t mx, my;
        MouseGetPosition(&mx, &my);
        uint32_t newMinY = (my >= 0) ? static_cast<uint32_t>(my) : 0;
        uint32_t newMaxY = static_cast<uint32_t>(my) + CURSOR_H;
        if (newMaxY > g_physFbHeight) newMaxY = g_physFbHeight;
        MarkDirtyRows(newMinY, newMaxY);
        CursorDraw(mx, my);
    }

    // Draw uptime clock overlay (only in legacy/non-WM mode — WM uses taskbar clock).
    if (!WmIsActive())
        CompositorDrawClock();

    // Flip: copy only dirty scanlines from backbuffer → MMIO framebuffer.
    if (g_backBuffer && g_dirtyMinY < g_dirtyMaxY)
    {
        uint32_t minY = g_dirtyMinY;
        uint32_t maxY = g_dirtyMaxY;
        if (maxY > g_physFbHeight) maxY = g_physFbHeight;
        g_dirtyMinY = 0xFFFFFFFFu;
        g_dirtyMaxY = 0;
        for (uint32_t y = minY; y < maxY; ++y)
        {
            __builtin_memcpy(
                const_cast<uint32_t*>(g_physFb + y * g_physFbStride),
                g_backBuffer + y * g_backBufStride,
                g_physFbWidth * 4);
        }
    }

    // Present-timing: record this frame's period + loop time and
    // periodically emit a summary on serial.  Additive diagnostics.
    uint64_t loopEndTick = g_lapicTickCount;
    uint64_t loopMs = loopEndTick - loopStartTick;
    if (periodMs > 0)
    {
        uint32_t idx = g_presentStatsIdx % PRESENT_STATS_WINDOW;
        g_presentPeriodMs[idx] = static_cast<uint32_t>(
            periodMs > 0xFFFFFFFFull ? 0xFFFFFFFFu : periodMs);
        g_presentLoopMs[idx]   = static_cast<uint32_t>(
            loopMs > 0xFFFFFFFFull ? 0xFFFFFFFFu : loopMs);
        ++g_presentStatsIdx;
        if (g_presentStatsFilled < PRESENT_STATS_WINDOW) ++g_presentStatsFilled;
    }
    PresentStatsReport();
}

static void CompositorThreadFn(void* /*arg*/)
{
    g_compositorProcess = ProcessCurrent();
    SerialPrintf("COMPOSITOR: thread started (%ux%u, %u processes)\n",
                 g_physFbWidth, g_physFbHeight, g_compositedCount);

    for (;;)
    {
        if (!g_compositorHalted)
            CompositorLoop();

        // Poll debug channel for incoming commands
        brook::DebugChannelPoll();

        // Block until either a process signals dirty (CompositorWake) or
        // the fallback interval elapses (ensures cursor updates even with
        // no dirty frames).
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

void CompositorWake()
{
    Process* p = g_compositorProcess;
    if (p && p->state == ProcessState::Blocked)
        SchedulerUnblock(p);
}

void CompositorMarkDirty()
{
    MarkAllDirty();
}

void CompositorUnregisterProcess(Process* proc)
{
    if (!proc) return;
    // If this process was the global input grabber, drop the reference so
    // we don't post-mortem push events into freed memory.
    CompositorClearInputGrabberIfMatches(proc);
    uint32_t count = __atomic_load_n(&g_compositedCount, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (__atomic_load_n(&g_compositedProcs[i], __ATOMIC_ACQUIRE) == proc)
        {
            __atomic_store_n(&g_compositedProcs[i], static_cast<Process*>(nullptr), __ATOMIC_RELEASE);
            break;
        }
    }
    // Clear compositor reference and allow the reaper to free pages.
    // DrainPostSwitch skips reapable for compositor-registered processes
    // to prevent PmmKillPid from freeing VFB pages mid-blit.
    __atomic_store_n(&proc->compositorRegistered, false, __ATOMIC_RELEASE);
    if (proc->state == ProcessState::Terminated)
        __atomic_store_n(&proc->reapable, true, __ATOMIC_RELEASE);
}

void CompositorWaitFrame()
{
    // Wait for the compositor to complete its current frame so VFB pages
    // are safe to free. We need the compositor to start a NEW frame
    // (meaning any in-progress blit from before our call has finished).
    uint64_t epoch = __atomic_load_n(&g_compositorEpoch, __ATOMIC_ACQUIRE);

    // Wake the compositor in case it's sleeping between frames
    CompositorWake();

    // Yield the CPU repeatedly to let the compositor thread run.
    // Check epoch between yields. Worst case: compositor frame is ~16ms.
    extern volatile uint64_t g_lapicTickCount;
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        if (__atomic_load_n(&g_compositorEpoch, __ATOMIC_ACQUIRE) != epoch)
            return;
        // Yield: block ourselves for 1ms so the compositor can run
        Process* self = ProcessCurrent();
        if (self)
        {
            self->wakeupTick = g_lapicTickCount + 1;
            SchedulerBlock(self);
        }
    }
    // Timeout (~50ms) — compositor may be halted, proceed anyway
}

void CompositorRemap(uint64_t fbPhys, uint32_t w, uint32_t h, uint32_t stridePixels)
{
    // Map the MMIO framebuffer from its physical address via the direct map.
    // We must NOT use TtyGetFramebuffer() here because it returns the
    // backbuffer pointer (already redirected), not the real MMIO address.
    g_physFb       = reinterpret_cast<volatile uint32_t*>(PhysToVirt(PhysicalAddress(fbPhys)).raw());
    g_physFbWidth  = w;
    g_physFbHeight = h;
    g_physFbStride = stridePixels;

    // Reallocate backbuffer for new resolution.
    // (old backbuffer leaked — acceptable for rare resolution changes)
    g_backBufStride = stridePixels;
    uint64_t bbSizeBytes = static_cast<uint64_t>(stridePixels) * h * 4;
    uint64_t bbPages = (bbSizeBytes + 4095) / 4096;
    VirtualAddress bbAddr = VmmAllocPages(bbPages, VMM_WRITABLE, MemTag::Device, 0);
    if (bbAddr)
    {
        g_backBuffer = reinterpret_cast<uint32_t*>(bbAddr.raw());
        // Seed from MMIO to preserve current screen content.
        for (uint32_t y = 0; y < h; ++y)
            __builtin_memcpy(
                g_backBuffer + y * stridePixels,
                const_cast<uint32_t*>(g_physFb + y * stridePixels),
                w * 4);
        TtyRedirectToBackbuffer(g_backBuffer);
    }

    // Update mouse cursor bounds to match new resolution.
    MouseSetBounds(w, h);
    MarkAllDirty();

    SerialPrintf("COMPOSITOR: remapped to %ux%u stride=%u\n", w, h, stridePixels);
}

void CompositorSetWallpaper(uint32_t* pixels, uint32_t w, uint32_t h)
{
    g_wallpaper = pixels;
    g_wallpaperWidth = w;
    g_wallpaperHeight = h;
    SerialPrintf("COMPOSITOR: wallpaper set %ux%u\n", w, h);
}

} // namespace brook
