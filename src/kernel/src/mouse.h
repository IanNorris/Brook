#pragma once

#include <stdint.h>

// PS/2 mouse driver.
//
// IRQ12 is routed through the I/O APIC at MOUSE_IRQ_VECTOR (44).
// 3-byte packet protocol: [status, deltaX, deltaY].
//
// mouse.cpp is compiled with -mgeneral-regs-only (ISR file).
//
// Lifecycle:
//   MouseInit() must be called after IoApicInit() and InputInit().

namespace brook {

// IRQ vector for mouse (ISA IRQ 12).
static constexpr uint8_t MOUSE_IRQ_VECTOR = 44;

// Mouse button bits (in status byte and in InputEvent.scanCode for button events).
static constexpr uint8_t MOUSE_BTN_LEFT   = (1 << 0);
static constexpr uint8_t MOUSE_BTN_RIGHT  = (1 << 1);
static constexpr uint8_t MOUSE_BTN_MIDDLE = (1 << 2);

// Initialise the PS/2 mouse: enable auxiliary device, install IRQ12 handler.
extern "C" void MouseInit();

// Returns true if MouseInit() has been called successfully.
extern "C" bool MouseIsAvailable();

// Get cursor position (updated by ISR, read by compositor).
extern "C" void MouseGetPosition(int32_t* x, int32_t* y);

// Set cursor bounds (call after display mode change).
extern "C" void MouseSetBounds(uint32_t maxX, uint32_t maxY);

// Get current button state bitmask.
extern "C" uint8_t MouseGetButtons();

// Set cursor position directly (used by absolute-positioning devices like virtio-tablet).
extern "C" void MouseSetPosition(int32_t x, int32_t y);

// Set button state directly (used by absolute-positioning devices).
extern "C" void MouseSetButtons(uint8_t buttons);

// Mark the mouse subsystem as available (called by non-PS/2 mouse drivers).
extern "C" void MouseSetAvailable(bool available);

} // namespace brook
