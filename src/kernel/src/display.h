#pragma once

// display.h — Abstract display device interface
//
// The kernel starts with the UEFI GOP framebuffer. A PCI display driver
// (e.g. bochs_display) can register itself to take over, enabling mode
// switching and proper hardware control.
//
// The compositor and TTY query the active display for the framebuffer
// address and dimensions.

#include <stdint.h>

namespace brook {

struct DisplayMode {
    uint32_t width;
    uint32_t height;
    uint32_t stride;        // bytes per row
    uint32_t bpp;           // bits per pixel (always 32 for now)
};

struct DisplayOps {
    const char* name;

    // Set display mode (resolution). Returns true on success.
    bool (*SetMode)(uint32_t width, uint32_t height);

    // Get current mode info.
    void (*GetMode)(DisplayMode* mode);

    // Get the virtual address of the framebuffer (mapped into kernel space).
    volatile uint32_t* (*GetFramebuffer)();

    // Get the physical base address of the framebuffer.
    uint64_t (*GetFramebufferPhys)();

    // Flush/sync framebuffer to display (no-op for direct-mapped linear FBs).
    void (*Flush)();
};

// Register a display driver. Replaces the current active display.
// Called by display driver modules during init.
extern "C" void DisplayRegister(const DisplayOps* ops);

// Get the active display. Returns the GOP fallback if no driver registered.
const DisplayOps* DisplayGetActive();

// Set display mode via the active driver.
bool DisplaySetMode(uint32_t width, uint32_t height);

// Query current display info.
void DisplayGetMode(DisplayMode* mode);

// Get framebuffer pointer from active display.
volatile uint32_t* DisplayGetFramebuffer();

// Get physical framebuffer address from active display.
uint64_t DisplayGetFramebufferPhys();

} // namespace brook
