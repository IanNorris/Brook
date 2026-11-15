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

// Apply DPI gain to desktop position only; callers publish raw input separately.
extern "C" void MouseMoveRelative(int32_t dx, int32_t dy);

// Set cursor position directly (used by absolute-positioning devices like virtio-tablet).
extern "C" void MouseSetPosition(int32_t x, int32_t y);

// Set button state directly (used by absolute-positioning devices).
extern "C" void MouseSetButtons(uint8_t buttons);

// Mark the mouse subsystem as available (called by non-PS/2 mouse drivers).
extern "C" void MouseSetAvailable(bool available);

} // namespace brook

namespace brook {
// Synaptics touchpad (absolute PS/2 mode, two-finger scroll). Boot default:
// enabled when the pad identifies as Synaptics; otherwise relative PS/2.
struct MouseTouchpadStatus {
    bool wanted, active, identified, clickpad, natural, agm, tap;
    uint32_t speed;
    uint32_t tapLeft, tapRight, tapDrags, tapDoubles;
    uint32_t tapRejectedPhysical, tapRejectedPalm, tapRejectedPhantom;
    uint32_t scrollLowZ, scrollRejected, gestureResets;
    const char* reason;
    uint8_t major, minor, model, mode, modeReadback, packetLen, relativeId;
    uint32_t caps, ext, ext0c, packets, resyncs, scrollTicks;
    uint32_t fingers[4];
    uint8_t rawValid;      // bit i: raw[i] read (identify, caps, ext 0x09, ext 0x0c, modes)
    uint8_t raw[5][3];
};
bool MouseTouchpadSet(bool synaptics);
bool MouseTouchpadProbe();
bool MouseTouchpadSetSpeed(uint32_t percent);
uint32_t MouseTouchpadSpeed();
void MouseTouchpadSetNatural(bool natural);
// Tap-to-click (Settings "Tap to click", [Desktop] touchpad_tap; default off).
void MouseTouchpadSetTap(bool enabled);
bool MouseTouchpadTap();
MouseTouchpadStatus MouseTouchpadGetStatus();
// Tap drag-window deadline in ApicTickCount() ms (0 = none). The BSP LAPIC
// timer calls MouseTouchpadTapPoll once it has passed.
extern volatile uint64_t g_touchpadTapDeadline;
void MouseTouchpadTapPoll(uint64_t nowMs);
}
