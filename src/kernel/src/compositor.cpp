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

// Compositor fallback interval: if no event-driven wakeup arrives within
// this many ms, the compositor wakes anyway (for cursor updates, etc.).
static constexpr uint32_t COMPOSITE_INTERVAL = 16; // ~60 Hz fallback

// Force-blit all processes every N ms regardless of dirty flag,
// so we see output even during init before DOOM signals dirty.
static constexpr uint32_t FORCE_BLIT_INTERVAL_MS = 500;
static uint64_t g_lastForceBlitTick = 0;

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
    if (!proc->fbVirtual || proc->fbVfbWidth == 0) return;
    if (!forceAll && !proc->fbDirty) return;
    proc->fbDirty = 0;

    const uint32_t* src = proc->fbVirtual;
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
        for (uint32_t y = 0; y < g_physFbHeight; ++y)
            for (uint32_t x = 0; x < g_physFbWidth; ++x)
                dstBase[y * dstStride + x] = 0x00001A3A;
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
        if (!w || !w->proc || !w->visible) continue;

        Process* p = w->proc;

        // Handle terminated processes
        if (p->state == ProcessState::Terminated)
        {
            WmDestroyWindow(sorted[i]);
            CompositorUnregisterProcess(p);
            continue;
        }

        // Update process destX/destY to match window client area
        p->fbDestX = w->clientX();
        p->fbDestY = w->clientY();

        if (p->fbVirtual && p->fbVfbWidth > 0)
            BlitProcessAt(p, w->clientX(), w->clientY(), true, w->upscale);
    }

    // 3. Draw window chrome (title bars, borders, buttons)
    if (g_backBuffer)
        WmRenderChrome(g_backBuffer, g_backBufStride, g_physFbWidth, g_physFbHeight);

    // 4. Handle mouse interaction
    CompositorHandleMouseWM();

    // 5. Route keyboard input to focused window
    InputEvent ev;
    while (InputPollEvent(&ev))
    {
        if (ev.type != InputEventType::KeyPress) continue;
        if (ev.ascii == 0) continue; // non-printable

        // Find the focused window
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

        char ch = ev.ascii;

        // Route to terminal if this window's process has one
        for (uint32_t ti = 0; ti < MAX_TERMINALS; ti++)
        {
            Terminal* t = TerminalGet(static_cast<int>(ti));
            if (t && t->child == focused->proc)
            {
                TerminalWriteInput(static_cast<int>(ti), &ch, 1);
                break;
            }
        }
    }
}

// Mouse state for WM interaction
static bool    g_wmDragging      = false;
static int     g_wmDragWindow    = -1;
static int16_t g_wmDragOffsetX   = 0;
static int16_t g_wmDragOffsetY   = 0;
static bool    g_wmLastBtnDown   = false;

static void CompositorHandleMouseWM()
{
    if (!MouseIsAvailable()) return;

    int32_t mx, my;
    MouseGetPosition(&mx, &my);
    uint8_t buttons = MouseGetButtons();
    bool btnDown = (buttons & 0x01) != 0; // left button

    if (btnDown && !g_wmLastBtnDown)
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
                    // TODO: send SIGTERM; for now terminate immediately
                    SerialPrintf("WM: close window %d '%s'\n", hit.windowIndex, w->title);
                }
                break;
            }
            case WmHitZone::MaximizeButton:
                WmToggleMaximize(hit.windowIndex);
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
            default:
                break;
            }
        }
    }
    else if (btnDown && g_wmDragging)
    {
        // Continue drag
        WmMoveWindow(g_wmDragWindow,
                     static_cast<int16_t>(mx - g_wmDragOffsetX),
                     static_cast<int16_t>(my - g_wmDragOffsetY));
    }
    else if (!btnDown)
    {
        g_wmDragging = false;
        g_wmDragWindow = -1;
    }

    g_wmLastBtnDown = btnDown;
}

static void CompositorLoop()
{
    if (!g_physFb)
        return;

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
            continue;
        }
        if (!p->fbVirtual || p->fbVfbWidth == 0)
            continue;

        BlitProcess(p, forceAll);
    }
    } // end if (g_compositedCount > 0)
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
    uint32_t count = __atomic_load_n(&g_compositedCount, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (__atomic_load_n(&g_compositedProcs[i], __ATOMIC_ACQUIRE) == proc)
        {
            __atomic_store_n(&g_compositedProcs[i], static_cast<Process*>(nullptr), __ATOMIC_RELEASE);
            break;
        }
    }
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
