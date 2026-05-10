# Input Subsystem Documentation

## Overview

Brook's input subsystem routes hardware events from PS/2 keyboard/mouse
through ISR handlers, per-device ring buffers, and into the compositor/WM
input pipeline.

## Files

| File | Lines | Purpose |
|------|-------|---------|
| input.cpp | ~120 | Input device registry, poll loop, waiter management |
| input.h | ~150 | InputDevice struct, ring buffer ops, InputEvent types |
| keyboard.cpp | ~600 | PS/2 keyboard driver, scancode→keycode, char buffer |
| keyboard.h | ~30 | Keyboard interface |
| mouse.cpp | ~410 | PS/2 mouse driver, packet state machine, bounds |
| mouse.h | ~30 | Mouse interface |

## Architecture

```
PS/2 Keyboard IRQ → KbdIrqHandler → InputDevicePush(ring)
PS/2 Mouse IRQ    → MouseIrqHandler → InputDevicePush(ring)
                                          ↓
                    InputPollEvent() ← compositor/syscall poll
                                          ↓
                    WM hit-test → per-window input queue → process
```

### Key Mechanisms
- **SPSC ring buffers**: Each InputDevice has a 256-slot ring (ISR producer,
  compositor consumer). Uses atomic head/tail with ACQUIRE/RELEASE ordering.
- **Waiter list**: Processes blocked on input register via `InputAddWaiter()`;
  ISR calls `InputWakeWaiters()` to unblock them. `pendingWakeup` flag
  prevents lost wakeups between add and block.
- **Keyboard**: Full PS/2 scancode set 1 with extended codes (0xE0 prefix).
  Tracks shift/ctrl/alt state. Separate 64-char text buffer for legacy
  `KbdGetChar()`.
- **Mouse**: 3-byte PS/2 protocol with sign extension from status byte.
  Clamped to configurable screen bounds.

## Audit Findings (2026-05-10)

### Known Issues
- **BRO-124**: `InputWakeWaiters()` reads `g_waiters[]` without synchronization
  — race with `InputRemoveWaiter()` can cause null deref or use-after-free
- Keyboard `KbdPeekChar()`/`KbdGetChar()` check-then-pop is non-atomic
  (mitigated: single consumer in practice)
- Keyboard buffer silently drops keys when 64-slot buffer is full
- Mouse delta sign extension manually applied rather than using int8_t cast
- `MouseSetBounds()` non-atomic update of g_maxX/g_maxY vs ISR clamp

### Overall Assessment
**Acceptable for hobby OS.** The SPSC ring buffer design is correct. Main risk
is the InputWakeWaiters race on SMP with concurrent process exit. Keyboard
buffer overflow is a usability annoyance, not a safety issue.
