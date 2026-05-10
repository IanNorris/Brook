# Display, Compositor & Window Manager Documentation

## Overview

Brook's display subsystem provides a kernel-mode compositor with window
management, dirty-region tracking, and double-buffered presentation.

## Files

| File | Lines | Purpose |
|------|-------|---------|
| display.cpp/h | ~200 | Display driver interface + GOP fallback |
| compositor.cpp/h | ~2100 | Double-buffered compositor thread, VFB blit, cursor |
| window.cpp/h | ~2200 | Z-ordered WM, chrome, taskbar, launcher UI |
| debug_overlay.cpp/h | ~220 | Kernel debug console ring buffer + TCP server |
| clock_overlay.cpp/h | ~60 | Uptime clock overlay |

## Architecture

```
Process VFBs (user-mapped framebuffers)
  → Compositor thread: dirty-region scan + blit to backbuffer
    → Window chrome rendering (title bar, buttons, resize handles)
    → Cursor overlay
    → memcpy backbuffer → physical MMIO framebuffer
  → Mouse/keyboard → WM hit-test → per-window input queues
```

### Key Mechanisms

- **Per-window VFBs**: Each window has a kernel-allocated virtual framebuffer
  mapped into the owning process's address space. Wayland clients attach
  wl_shm buffers; waylandd copies into VFB via WM_BLIT_VFB syscall.
- **Dirty tracking**: `proc->fbDirty` flag + per-window dirty rects.
  Compositor skips windows with no changes.
- **Deferred page free**: VFB resize frees old pages after compositor epoch
  advances, preventing use-after-free during mid-blit resize.
- **Z-ordering**: Insertion-sorted array with sticky focus/top rules.
- **Launcher**: Loads `.rc` shortcut files from `/boot/SHORTCUTS/`,
  renders icon grid with scroll support.

## Audit Findings (2026-05-10)

### Known Issues
- **BRO-120**: Deferred page free buffer (128 slots) can overflow, leaking pages
- **BRO-121**: `fbDirty` flag is non-atomic — missed frames on SMP
- **BRO-122**: Per-window input queue (64 slots) drops keyboard events when full
- Epoch-based frame sync doesn't handle halted compositor state
- Alpha-blend row optimization assumes entire row is opaque if first pixel is
- Cursor save buffer lacks cw/ch bounds clamp against CURSOR_MAX
- Launcher title fallback path may leave buffer uninitialized

### Overall Assessment
**Moderate correctness.** The compositor architecture is well-designed with
proper dirty-region optimization. Main risks are SMP races on non-atomic
flags and fixed-size buffers that silently drop data under load.
