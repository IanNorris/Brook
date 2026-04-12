// bochs_display_mod.cpp — Bochs/stdvga display driver for QEMU
//
// Drives the QEMU standard VGA device (PCI 1234:1111, class 03:00).
// Uses the Bochs VBE Dispi interface (I/O ports 0x01CE/0x01CF) for mode
// setting. The kernel's TtyRemap handles framebuffer virtual memory mapping.
//
// This replaces the UEFI GOP framebuffer with a driver that supports
// runtime resolution changes.

#include "module_abi.h"
#include "pci.h"
#include "display.h"
#include "serial.h"
#include "kprintf.h"
#include "tty.h"
#include "compositor.h"

MODULE_IMPORT_SYMBOL(PciFindDevice);
MODULE_IMPORT_SYMBOL(PciEnableMemSpace);
MODULE_IMPORT_SYMBOL(DisplayRegister);
MODULE_IMPORT_SYMBOL(SerialPrintf);
MODULE_IMPORT_SYMBOL(SerialPuts);
MODULE_IMPORT_SYMBOL(KPrintf);
MODULE_IMPORT_SYMBOL(TtyRemap);
MODULE_IMPORT_SYMBOL(CompositorRemap);
MODULE_IMPORT_SYMBOL(TtyGetFramebuffer);

using namespace brook;

// ---------------------------------------------------------------------------
// Bochs VBE Dispi registers (I/O port interface)
// ---------------------------------------------------------------------------

static constexpr uint16_t VBE_DISPI_IOPORT_INDEX = 0x01CE;
static constexpr uint16_t VBE_DISPI_IOPORT_DATA  = 0x01CF;

static constexpr uint16_t VBE_DISPI_INDEX_ID          = 0x00;
static constexpr uint16_t VBE_DISPI_INDEX_XRES        = 0x01;
static constexpr uint16_t VBE_DISPI_INDEX_YRES        = 0x02;
static constexpr uint16_t VBE_DISPI_INDEX_BPP         = 0x03;
static constexpr uint16_t VBE_DISPI_INDEX_ENABLE      = 0x04;
static constexpr uint16_t VBE_DISPI_INDEX_VIRT_WIDTH  = 0x06;
static constexpr uint16_t VBE_DISPI_INDEX_VIRT_HEIGHT = 0x07;
static constexpr uint16_t VBE_DISPI_INDEX_X_OFFSET    = 0x08;
static constexpr uint16_t VBE_DISPI_INDEX_Y_OFFSET    = 0x09;

static constexpr uint16_t VBE_DISPI_DISABLED    = 0x00;
static constexpr uint16_t VBE_DISPI_ENABLED     = 0x01;
static constexpr uint16_t VBE_DISPI_LFB_ENABLED = 0x40;

static inline void VbeWrite(uint16_t index, uint16_t value)
{
    __asm__ volatile("outw %0, %1" :: "a"(index), "Nd"(VBE_DISPI_IOPORT_INDEX));
    __asm__ volatile("outw %0, %1" :: "a"(value), "Nd"(VBE_DISPI_IOPORT_DATA));
}

static inline uint16_t VbeRead(uint16_t index)
{
    __asm__ volatile("outw %0, %1" :: "a"(index), "Nd"(VBE_DISPI_IOPORT_INDEX));
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(VBE_DISPI_IOPORT_DATA));
    return val;
}

// ---------------------------------------------------------------------------
// Driver state
// ---------------------------------------------------------------------------

static PciDevice g_pciDev;
static uint64_t  g_fbPhys   = 0;     // BAR0 physical base
static uint32_t  g_width    = 0;
static uint32_t  g_height   = 0;

// ---------------------------------------------------------------------------
// Set display mode via VBE Dispi interface
// ---------------------------------------------------------------------------

static bool SetModeVBE(uint32_t w, uint32_t h)
{
    // Disable display before changing mode
    VbeWrite(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);

    // Set resolution and BPP
    VbeWrite(VBE_DISPI_INDEX_XRES, static_cast<uint16_t>(w));
    VbeWrite(VBE_DISPI_INDEX_YRES, static_cast<uint16_t>(h));
    VbeWrite(VBE_DISPI_INDEX_BPP,  32);
    VbeWrite(VBE_DISPI_INDEX_VIRT_WIDTH, static_cast<uint16_t>(w));
    VbeWrite(VBE_DISPI_INDEX_VIRT_HEIGHT, static_cast<uint16_t>(h));
    VbeWrite(VBE_DISPI_INDEX_X_OFFSET, 0);
    VbeWrite(VBE_DISPI_INDEX_Y_OFFSET, 0);

    // Enable with linear framebuffer
    VbeWrite(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    // Verify mode took effect
    uint16_t actualW = VbeRead(VBE_DISPI_INDEX_XRES);
    uint16_t actualH = VbeRead(VBE_DISPI_INDEX_YRES);
    if (actualW != w || actualH != h) {
        SerialPrintf("bochs: mode set failed — requested %ux%u, got %ux%u\n",
                     w, h, actualW, actualH);
        return false;
    }

    g_width  = w;
    g_height = h;

    SerialPrintf("bochs: VBE mode set to %ux%u @ 32bpp\n", w, h);
    return true;
}

// ---------------------------------------------------------------------------
// DisplayOps implementation
// ---------------------------------------------------------------------------

static bool BochsSetMode(uint32_t w, uint32_t h)
{
    if (!SetModeVBE(w, h)) return false;

    // Remap framebuffer at new dimensions (kernel handles VMM mapping)
    TtyRemap(g_fbPhys, w, h, w);
    CompositorRemap(g_fbPhys, w, h, w);
    return true;
}

static void BochsGetMode(DisplayMode* mode)
{
    mode->width  = g_width;
    mode->height = g_height;
    mode->stride = g_width * 4;
    mode->bpp    = 32;
}

static volatile uint32_t* BochsGetFramebuffer()
{
    // Use TtyGetFramebuffer to get the kernel-mapped virtual address
    uint32_t* pixels;
    uint32_t w, h, stride;
    if (TtyGetFramebuffer(&pixels, &w, &h, &stride))
        return reinterpret_cast<volatile uint32_t*>(pixels);
    return nullptr;
}

static uint64_t BochsGetFramebufferPhys()
{
    return g_fbPhys;
}

static void BochsFlush() { /* linear FB — no flush needed */ }

static const DisplayOps g_bochsOps = {
    "bochs-vga",
    BochsSetMode,
    BochsGetMode,
    BochsGetFramebuffer,
    BochsGetFramebufferPhys,
    BochsFlush,
};

// ---------------------------------------------------------------------------
// Module init/exit
// ---------------------------------------------------------------------------

static int BochsDisplayModuleInit()
{
    SerialPuts("bochs_display: init\n");

    // Find the QEMU stdvga device (vendor 1234, device 1111)
    if (!PciFindDevice(0x1234, 0x1111, g_pciDev)) {
        SerialPuts("bochs_display: device 1234:1111 not found\n");
        return -1;
    }

    SerialPrintf("bochs_display: found at %02x:%02x.%x\n",
                 g_pciDev.bus, g_pciDev.dev, g_pciDev.fn);

    // Enable memory space access
    PciEnableMemSpace(g_pciDev);

    // BAR0 = linear framebuffer
    g_fbPhys = PciBarMemBase32(g_pciDev.bar[0]);
    if (PciBarIs64(g_pciDev.bar[0])) {
        g_fbPhys |= static_cast<uint64_t>(g_pciDev.bar[1]) << 32;
    }

    SerialPrintf("bochs_display: BAR0 at phys 0x%lx\n", g_fbPhys);

    // Check VBE Dispi interface is present
    uint16_t id = VbeRead(VBE_DISPI_INDEX_ID);
    if (id < 0xB0C0 || id > 0xB0CF) {
        SerialPrintf("bochs_display: VBE Dispi ID 0x%x — not Bochs VGA\n", id);
        return -1;
    }
    SerialPrintf("bochs_display: VBE Dispi ID 0x%x\n", id);

    // Read current mode (set by UEFI GOP)
    g_width  = VbeRead(VBE_DISPI_INDEX_XRES);
    g_height = VbeRead(VBE_DISPI_INDEX_YRES);

    KPrintf("bochs_display: %ux%u @ 32bpp, FB phys 0x%lx\n",
            g_width, g_height, g_fbPhys);

    // Register with the display subsystem
    DisplayRegister(&g_bochsOps);

    // Remap TTY and compositor to use our framebuffer
    // (same physical memory as GOP initially, but now under driver control)
    TtyRemap(g_fbPhys, g_width, g_height, g_width);
    CompositorRemap(g_fbPhys, g_width, g_height, g_width);

    return 0;
}

static void BochsDisplayModuleExit()
{
    SerialPuts("bochs_display: exit\n");
}

DECLARE_MODULE("bochs_display", BochsDisplayModuleInit, BochsDisplayModuleExit,
               "Bochs/QEMU stdvga display driver (PCI 1234:1111)");
