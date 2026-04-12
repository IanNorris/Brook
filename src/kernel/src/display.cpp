// display.cpp — Display device abstraction
//
// Manages the active display driver. Defaults to the UEFI GOP framebuffer
// set up during boot. PCI display drivers (loaded as modules) can register
// themselves to take over.

#include "display.h"
#include "tty.h"
#include "serial.h"

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
    uint32_t* fb;
    uint32_t w, h, stride;
    TtyGetFramebuffer(&fb, &w, &h, &stride);
    mode->width  = w;
    mode->height = h;
    mode->stride = stride;
    mode->bpp    = 32;
}

static volatile uint32_t* GopGetFramebuffer()
{
    uint32_t* fb;
    uint32_t w, h, stride;
    TtyGetFramebuffer(&fb, &w, &h, &stride);
    return reinterpret_cast<volatile uint32_t*>(fb);
}

static uint64_t GopGetFramebufferPhys()
{
    uint64_t physBase;
    uint32_t w, h, stride;
    if (TtyGetFramebufferPhys(&physBase, &w, &h, &stride))
        return physBase;
    return 0;
}

static void GopFlush() { /* linear FB, no-op */ }

static const DisplayOps g_gopOps = {
    "gop",
    GopSetMode,
    GopGetMode,
    GopGetFramebuffer,
    GopGetFramebufferPhys,
    GopFlush,
};

// ---------------------------------------------------------------------------
// Active display
// ---------------------------------------------------------------------------

static const DisplayOps* g_activeDisplay = &g_gopOps;

void DisplayRegister(const DisplayOps* ops)
{
    if (!ops || !ops->name) return;
    SerialPrintf("DISPLAY: registering '%s' (replacing '%s')\n",
                 ops->name, g_activeDisplay->name);
    g_activeDisplay = ops;
}

const DisplayOps* DisplayGetActive()
{
    return g_activeDisplay;
}

bool DisplaySetMode(uint32_t width, uint32_t height)
{
    return g_activeDisplay->SetMode(width, height);
}

void DisplayGetMode(DisplayMode* mode)
{
    g_activeDisplay->GetMode(mode);
}

volatile uint32_t* DisplayGetFramebuffer()
{
    return g_activeDisplay->GetFramebuffer();
}

uint64_t DisplayGetFramebufferPhys()
{
    return g_activeDisplay->GetFramebufferPhys();
}

} // namespace brook
