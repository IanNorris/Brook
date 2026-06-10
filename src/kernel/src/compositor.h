#pragma once

#include <stdint.h>

namespace brook {

struct Process;

// Initialise the compositor (caches physical FB info).
// Must be called after TtyInit().
void CompositorInit();

// Get physical framebuffer dimensions.
extern "C" void CompositorGetPhysDims(uint32_t* w, uint32_t* h);

// Get physical framebuffer pointer and stride (for kernel threads that
// need direct FB access, e.g. clock overlay).
volatile uint32_t* CompositorGetPhysFb(uint32_t* stride);

// Smoothed compositor present rate (frames per second), for the taskbar readout.
uint32_t CompositorGetFps();

// Allocate a per-process virtual framebuffer of size vfbWidth × vfbHeight.
// The process sees this as its screen. The compositor blits it 1:1 at
// (destX, destY) on the physical framebuffer, clipping to screen bounds.
// vfbWidth=0 or vfbHeight=0 means no compositing.
bool CompositorSetupProcess(Process* proc, int16_t destX, int16_t destY,
                             uint32_t vfbWidth, uint32_t vfbHeight, uint8_t scale = 1);

// Resize an existing process VFB to newWidth x newHeight.
// Allocates a new buffer, waits for the compositor to finish the current
// frame, swaps the pointer atomically, then leaks the old VFB pages
// (acceptable for rare resize events — PmmKillPid will reclaim on exit).
// The associated WM window is left untouched; caller should also call
// WmResizeWindow() to update on-screen dimensions.
bool CompositorResizeVfb(Process* proc, uint32_t newWidth, uint32_t newHeight);

// Start the compositor kernel thread. Must be called after CompositorInit()
// and after all processes have been spawned (or at least after SchedulerInit).
void CompositorStartThread();

// Halt the compositor — called during panic to prevent overwriting the QR code.
void CompositorHalt();

// Wake the compositor thread immediately (e.g. when a process signals a dirty frame).
extern "C" void CompositorWake();

// Mark the backbuffer as dirty so the next compositor loop copies it to MMIO.
// Called by the TTY after rendering text into the backbuffer.
void CompositorMarkDirty();

// Begin a WM drag operation for a window at the current mouse position.
// Used by Wayland clients via xdg_toplevel.move when they draw their own chrome.
bool CompositorBeginWindowMove(int windowIdx);

// Begin an interactive WM resize using the current pointer position.
bool CompositorBeginWindowResize(int windowIdx, bool left, bool right,
                                 bool top, bool bottom);

// Unregister a process from the compositor (called before the Process is freed).
void CompositorUnregisterProcess(Process* proc);

// Wait until any in-progress compositor frame completes.
// Used by ProcessDestroy to ensure VFB pages aren't freed mid-blit.
void CompositorWaitFrame();

// Defer freeing compositor-read kernel pages until any in-flight frame that may
// have snapshotted the pointer has retired.
void CompositorDeferFreePages(uint64_t virtAddr, uint64_t pageCount);

// Hot-swap the physical framebuffer (called by display driver on mode change).
// Must be called AFTER TtyRemap (uses TtyGetFramebuffer to get the new mapping).
// stridePixels is the row stride in pixels (not bytes).
extern "C" void CompositorRemap(uint64_t fbPhys, uint32_t w, uint32_t h,
                                uint32_t stridePixels);

// Set wallpaper pixel data. Takes ownership of the pointer (caller must not free).
// Pixel format is XRGB (0x00RRGGBB), row-major, w pixels per row.
void CompositorSetWallpaper(uint32_t* pixels, uint32_t w, uint32_t h);

// Input "grabber" — a userspace display server (e.g. waylandd) registers
// itself as the global input sink so it sees every keyboard/mouse event,
// regardless of which window the kernel WM thinks owns the click.  This
// is what lets a Wayland compositor route input to its own clients via
// wl_pointer / wl_keyboard.  enable=1 sets ProcessCurrent() as grabber;
// enable=0 clears it (only succeeds if caller is the current grabber).
// Returns true on success.
bool CompositorSetInputGrabber(Process* proc, bool enable);
// Cleared automatically when the grabber process exits.
void CompositorClearInputGrabberIfMatches(Process* proc);

// Hide/show the compositor-owned default mouse cursor. This is intentionally
// only visibility state; Wayland cursor surface pixels are a separate concern.
bool CompositorSetCursorVisible(Process* proc, bool visible);

// Upload a custom ARGB8888 cursor image. Pixels are copied into a
// kernel-side buffer. Max 64×64. hotX/hotY are the click-point offset.
// Pass w=0 to reset to the built-in arrow cursor.
bool CompositorSetCursorImage(const uint32_t* pixels, uint32_t w, uint32_t h,
                              int32_t hotX, int32_t hotY);

// --- Scanout interface (Phase 1 of usermode compositor migration) ---
// These allow a privileged userspace process to take over compositing duties.

// Map the compositor backbuffer into the calling process's address space.
// Returns the user-space virtual address, or 0 on failure.
// Also writes screen dimensions and stride to the output struct.
struct ScanoutMapResult {
    uint64_t userAddr;    // mapped address in caller's page table
    uint32_t width;       // screen width in pixels
    uint32_t height;      // screen height in pixels
    uint32_t stride;      // row stride in pixels
    uint32_t bufferBytes; // total buffer size
};
uint64_t CompositorScanoutMap(Process* proc, ScanoutMapResult* out);

// Flip dirty rows from the (userspace-written) backbuffer to MMIO framebuffer.
// Only copies rows in [minY, maxY). Returns 0 on success.
int CompositorScanoutFlip(uint32_t minY, uint32_t maxY);

// --- Phase 2: Window state query interface for usermode compositor ---

// Descriptor returned by WM_GET_WINDOWS syscall (523).
// Packed for user-space consumption.
struct WmWindowDesc {
    uint32_t wmId;
    int16_t  x, y;
    uint16_t clientW, clientH;
    uint32_t zOrder;
    uint32_t flags;       // bit 0=focused, 1=visible, 2=minimized, 3=noChrome, 4=focusable
    uint64_t vfbUserAddr; // user-mapped VFB address (in compositor's address space)
    uint32_t vfbStride;   // pixels per row
    uint32_t vfbBytes;    // total VFB size
    uint16_t pid;
    char     title[64];
};

static constexpr uint32_t WM_DESC_FOCUSED    = (1u << 0);
static constexpr uint32_t WM_DESC_VISIBLE    = (1u << 1);
static constexpr uint32_t WM_DESC_MINIMIZED  = (1u << 2);
static constexpr uint32_t WM_DESC_NO_CHROME  = (1u << 3);
static constexpr uint32_t WM_DESC_FOCUSABLE  = (1u << 4);

// Fill an array of window descriptors for the userspace compositor.
// Maps each window's VFB into the calling process if not already mapped.
// Returns the number of windows written.
uint32_t CompositorGetWindows(Process* caller, WmWindowDesc* out, uint32_t maxCount);

// Map a specific window's VFB into the compositor process (read-only).
// Returns user-space address, or 0 on failure.
uint64_t CompositorMapWindowVfb(Process* caller, uint32_t wmId);

} // namespace brook
