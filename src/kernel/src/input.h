#pragma once

#include <stdint.h>

// Generic input device interface.
//
// Abstracts keyboard (and future mouse/gamepad) hardware behind a
// common API so higher layers don't care whether input comes from PS/2,
// USB HID, or virtio-input.
//
// Each input driver creates an InputDevice and calls InputRegister().
// The kernel's keyboard subsystem (keyboard.h) consumes events from
// all registered input devices.
//
// Event model:
//   - Key press/release events carry a HID-style scan code and ASCII.
//   - A small per-device ring buffer queues events between IRQ and consumer.

namespace brook {

// Input event types.
enum class InputEventType : uint8_t {
    KeyPress        = 0,
    KeyRelease      = 1,
    MouseMove       = 2,
    MouseButtonDown = 3,
    MouseButtonUp   = 4,
};

// A single input event.
struct InputEvent {
    InputEventType type;
    uint8_t  scanCode;   // Hardware scan code (driver-dependent)
    char     ascii;      // Translated ASCII character (0 if non-printable)
    uint8_t  modifiers;  // Bitmask of active modifiers at event time
};

// Modifier bits.
static constexpr uint8_t INPUT_MOD_LSHIFT   = (1 << 0);
static constexpr uint8_t INPUT_MOD_RSHIFT   = (1 << 1);
static constexpr uint8_t INPUT_MOD_CTRL     = (1 << 2);
static constexpr uint8_t INPUT_MOD_ALT      = (1 << 3);
static constexpr uint8_t INPUT_MOD_CAPSLOCK = (1 << 4);

static constexpr uint8_t INPUT_MOD_SHIFT = (INPUT_MOD_LSHIFT | INPUT_MOD_RSHIFT);

// Per-device event ring buffer size (must be power of 2).
static constexpr uint32_t INPUT_RING_SIZE = 64;

// Forward declaration.
struct InputDevice;

// Operations provided by an input driver.
struct InputDeviceOps {
    // Human-readable driver name (e.g. "ps2_kbd", "usb_kbd").
    const char* name;

    // Poll for events (optional; nullptr if interrupt-driven).
    // Called periodically by the input subsystem.
    void (*poll)(InputDevice* dev);
};

// An input device instance.
// Drivers allocate this (static or kmalloc) and call InputRegister().
struct InputDevice {
    const InputDeviceOps* ops;

    // Ring buffer — written by driver (ISR), read by consumer.
    InputEvent ring[INPUT_RING_SIZE];
    volatile uint32_t head;  // Next write index (ISR)
    volatile uint32_t tail;  // Next read index (consumer)

    // Driver-private data.
    void* priv;
};

// Maximum number of registered input devices.
static constexpr uint32_t INPUT_MAX_DEVICES = 8;

// ---------------------------------------------------------------------------
// Input subsystem API (kernel side)
// ---------------------------------------------------------------------------

// Initialise the input subsystem.  Call once at boot.
void InputInit();

// Register an input device.  Returns true on success.
bool InputRegister(InputDevice* dev);

// Dequeue the next event from any registered device.
// Returns true if an event was available, false if all queues are empty.
bool InputPollEvent(InputEvent* out);

// Blocking: spin until an event is available, then return it.
InputEvent InputWaitEvent();

// Check if any input device has queued events.
bool InputHasEvents();

// ---------------------------------------------------------------------------
// Driver-side helpers — call from ISR or driver code to enqueue events
// ---------------------------------------------------------------------------

// Push an event into a device's ring buffer.
// Safe to call from ISR context.
inline void InputDevicePush(InputDevice* dev, const InputEvent& ev)
{
    uint32_t next = (dev->head + 1) & (INPUT_RING_SIZE - 1);
    if (next == dev->tail) return; // buffer full, drop event
    dev->ring[dev->head] = ev;
    dev->head = next;
}

// Pop an event from a device's ring buffer.
// Returns true if an event was dequeued.
inline bool InputDevicePop(InputDevice* dev, InputEvent* out)
{
    if (dev->tail == dev->head) return false;
    *out = dev->ring[dev->tail];
    dev->tail = (dev->tail + 1) & (INPUT_RING_SIZE - 1);
    return true;
}

} // namespace brook
