#include "framebuffer_fence.h"
// display.cpp — Display device abstraction
//
// Manages the active display driver. Defaults to the UEFI GOP framebuffer
// set up during boot. PCI display drivers (loaded as modules) can register
// themselves to take over.

#include "display.h"
#include "compositor.h"
#include "display_power.h"
#include "module.h"
#include "tty.h"
#include "serial.h"
#include "sync/kmutex.h"

namespace brook {

// ---------------------------------------------------------------------------
// GOP fallback — uses the framebuffer set up by TtyInit from UEFI GOP.
// ---------------------------------------------------------------------------

static bool GopSetMode(uint32_t /*w*/, uint32_t /*h*/)
{
    // GOP doesn't support runtime mode switching after ExitBootServices.
    return false;
}

static void GopGetMode(DisplayMode* mode)
{
    *mode = {};
    TtyGetFramebufferDimensions(&mode->width, &mode->height, &mode->stride);
    mode->bpp    = 32;
}

static volatile uint32_t* GopGetFramebuffer()
{
    uint32_t* fb = nullptr;
    return TtyGetFramebuffer(&fb, nullptr, nullptr, nullptr) ? fb : TtyGetFramebufferMmio();
}

static uint64_t GopGetFramebufferPhys()
{
    uint64_t physBase;
    uint32_t w, h, stride;
    if (TtyGetFramebufferPhys(&physBase, &w, &h, &stride))
        return physBase;
    return 0;
}

static void GopFlush(uint32_t /*minY*/, uint32_t /*maxY*/) { /* linear FB, no-op */ }

static const DisplayOps g_gopOps = {
    "gop",
    GopSetMode,
    GopGetMode,
    GopGetFramebuffer,
    GopGetFramebufferPhys,
    GopFlush,
    nullptr,  // PanicPresent: GOP FB is directly scanned out — no-op
};

// ---------------------------------------------------------------------------
// Active display
// ---------------------------------------------------------------------------

static const DisplayOps* g_activeDisplay = &g_gopOps;
static const DisplayCursorOps* g_cursorOps = nullptr;
static KMutex g_framebufferChangeLock{};
static const DisplayOps* g_managedFramebufferOwner = nullptr;

bool DisplayBeginFramebufferChange(bool managed, const DisplayOps* expected)
{
    KMutexLock(&g_framebufferChangeLock);
    const auto* active = DisplayGetActive();
    const bool owned = g_managedFramebufferOwner != nullptr;
    const bool allowed = managed
        ? (expected ? owned && expected == g_managedFramebufferOwner && active == expected
                    : !owned && active == &g_gopOps)
        : !owned;
    if (!allowed) KMutexUnlock(&g_framebufferChangeLock);
    return allowed;
}

void DisplayEndFramebufferChange(const DisplayOps* claim)
{
    if (claim && claim == DisplayGetActive()) g_managedFramebufferOwner = claim;
    KMutexUnlock(&g_framebufferChangeLock);
}


void DisplayRegister(const DisplayOps* ops)
{
    if (!ops || !ops->name) return;
    // The active table can be invoked by the panic path as well as the
    // compositor. Its provider must survive until kernel shutdown.
    const uintptr_t table = reinterpret_cast<uintptr_t>(ops);
    if (!ModuleRetainAddresses(&table, 1)) return;
    CompositorLockScanoutForDisplayChange();
    SerialPrintf("DISPLAY: registering '%s' (replacing '%s')\n",
                 ops->name, DisplayGetActive()->name);
    const auto* cursor = __atomic_load_n(&g_cursorOps, __ATOMIC_ACQUIRE);
    if (cursor && cursor->display != ops) cursor->Prepare(nullptr, 0, 0);
    __atomic_store_n(&g_activeDisplay, ops, __ATOMIC_RELEASE);
    CompositorUnlockScanoutForDisplayChange();
}

const DisplayOps* DisplayGetActive()
{
    return __atomic_load_n(&g_activeDisplay, __ATOMIC_ACQUIRE);
}

void DisplayRegisterCursor(const DisplayCursorOps* ops)
{
    if (!ops || !ops->display || !ops->Prepare || !ops->Commit ||
        ops->display != DisplayGetActive()) return;
    const uintptr_t table = reinterpret_cast<uintptr_t>(ops);
    if (ModuleRetainAddresses(&table, 1))
        __atomic_store_n(&g_cursorOps, ops, __ATOMIC_RELEASE);
}

bool DisplayPrepareCursor(const CursorImage* image, int32_t x, int32_t y)
{
    const auto* ops = __atomic_load_n(&g_cursorOps, __ATOMIC_ACQUIRE);
    return ops && ops->display == DisplayGetActive() && ops->Prepare(image, x, y);
}

void DisplayCommitCursor()
{
    const auto* ops = __atomic_load_n(&g_cursorOps, __ATOMIC_ACQUIRE);
    if (ops && ops->display == DisplayGetActive()) ops->Commit();
}

bool DisplayUsesFirmwareFramebuffer() { return DisplayGetActive()==&g_gopOps; }

bool DisplaySetMode(uint32_t width, uint32_t height)
{
    return DisplayGetActive()->SetMode(width, height);
}

void DisplayGetMode(DisplayMode* mode)
{
    DisplayGetActive()->GetMode(mode);
}

volatile uint32_t* DisplayGetFramebuffer()
{
    return DisplayGetActive()->GetFramebuffer();
}

uint64_t DisplayGetFramebufferPhys()
{
    return DisplayGetActive()->GetFramebufferPhys();
}

void DisplayFlush(uint32_t minY, uint32_t maxY)
{
    FramebufferWriteFence();
    DisplayGetActive()->Flush(minY, maxY);
}

void DisplayPanicPresent()
{
    DisplayPowerPanicRestore();
    FramebufferWriteFence();
    if (const auto* active=DisplayGetActive(); active && active->PanicPresent)
        active->PanicPresent();
}

// ---------------------------------------------------------------------------
// 3D-acceleration status (set by the virtio-gpu driver once its host 3D path
// is proven; read by the taskbar GPU badge).
// ---------------------------------------------------------------------------

static bool g_display3dActive = false;

extern "C" void DisplaySet3DActive(bool active)
{
    if (active != g_display3dActive)
        SerialPrintf("DISPLAY: 3D acceleration %s\n", active ? "ACTIVE" : "inactive");
    g_display3dActive = active;
}

bool DisplayIs3DActive()
{
    return g_display3dActive;
}

} // namespace brook
