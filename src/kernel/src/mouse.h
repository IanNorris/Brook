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

// Bound the pointer to the union of output rectangles (desktop coordinates,
// at or right/below of 0,0): it moves to the nearest pixel of any output, so
// it cannot rest where no output shows it. One rectangle {0,0,w,h} is
// MouseSetBounds(w, h).
struct OutputRect;
void MouseSetOutputBounds(const OutputRect* rects, uint32_t count);

// Get current button state bitmask.
extern "C" uint8_t MouseGetButtons();

// Apply DPI gain to desktop position only; callers publish raw input separately.
extern "C" void MouseMoveRelative(int32_t dx, int32_t dy);

// Set cursor position directly (used by absolute-positioning devices like virtio-tablet).
extern "C" void MouseSetPosition(int32_t x, int32_t y);

// Set button state directly (used by absolute-positioning devices).
extern "C" void MouseSetButtons(uint8_t buttons);

// Mark the mouse subsystem as available (called by non-PS/2 mouse drivers).
extern "C" void MouseSetUsbButtons(uint8_t buttons);
extern "C" void MouseSetUsbAvailable(bool available);
extern "C" void MouseSetAvailable(bool available);

} // namespace brook

namespace brook {
// Synaptics touchpad (absolute PS/2 mode, two-finger scroll). Boot default:
// enabled when the pad identifies as Synaptics; otherwise relative PS/2.
constexpr int MouseTouchpadRawReplies = 7;
struct MouseTouchpadStatus {
    bool wanted, active, identified, clickpad, natural, agm, tap;
    uint32_t speed;
    uint32_t tapLeft, tapRight, tapDrags, tapDoubles;
    uint32_t tapRejectedPhysical, tapRejectedPalm, tapRejectedPhantom;
    uint32_t scrollLowZ, scrollRejected, gestureResets;
    // Clickpad press decisions (synaptics_touchpad.h ChooseClick).
    uint32_t clicksLeft, clicksRight, clicksRightArea, clicksTwoFinger, clicksThumbLeft;
    uint32_t clicksStalePos, clicksNoPos;
    int32_t xMin, xMax, yMin, yMax;     // coordinate range (nominal unless reported)
    bool rangeReportedMax, rangeReportedMin, softButtons, traceOn;
    int32_t buttonTopY, buttonSplitX;   // button strip: y below top; right at x >= split
    uint32_t traceTotal;
    const char* reason;
    uint8_t major, minor, model, mode, modeReadback, packetLen, relativeId;
    uint32_t caps, ext, ext0c, packets, resyncs, scrollTicks;
    uint32_t fingers[4];
    uint8_t rawValid;      // bit i: raw[i] read (identify, caps, ext 0x09, ext 0x0c, modes, max 0x0d, min 0x0f)
    uint8_t raw[MouseTouchpadRawReplies][3];
};
// Raw packet trace for the debug bridge. Start clears the ring and records
// every 6-byte packet with its time and the physical buttons the gesture code
// reported; Stop ends recording and returns the ring, oldest first from
// entries[first] (wrapping), valid until the next Start.
struct MouseTouchpadTraceEntry {
    uint32_t ms;           // since Start
    uint8_t bytes[6];
    uint8_t buttons;       // gesture physical buttons after this packet (bit0 left, bit1 right)
    uint8_t reserved;
};
constexpr uint32_t MouseTouchpadTraceCapacity = 2048;   // ~13 s at 160 packets/s (two fingers with AGM)
struct MouseTouchpadTraceView {
    const MouseTouchpadTraceEntry* entries;
    uint32_t capacity, first, count, total;
};
void MouseTouchpadTraceStart();
MouseTouchpadTraceView MouseTouchpadTraceStop();
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
