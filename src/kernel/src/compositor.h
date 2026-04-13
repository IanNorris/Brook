#pragma once

#include <stdint.h>

namespace brook {

struct Process;

// Initialise the compositor (caches physical FB info).
// Must be called after TtyInit().
void CompositorInit();

// Get physical framebuffer dimensions.
void CompositorGetPhysDims(uint32_t* w, uint32_t* h);

// Get physical framebuffer pointer and stride (for kernel threads that
// need direct FB access, e.g. clock overlay).
volatile uint32_t* CompositorGetPhysFb(uint32_t* stride);

// Allocate a per-process virtual framebuffer of size vfbWidth × vfbHeight.
// The process sees this as its screen. The compositor blits it 1:1 at
// (destX, destY) on the physical framebuffer, clipping to screen bounds.
// vfbWidth=0 or vfbHeight=0 means no compositing.
bool CompositorSetupProcess(Process* proc, int16_t destX, int16_t destY,
                             uint32_t vfbWidth, uint32_t vfbHeight, uint8_t scale = 1);

// Start the compositor kernel thread. Must be called after CompositorInit()
// and after all processes have been spawned (or at least after SchedulerInit).
void CompositorStartThread();

// Halt the compositor — called during panic to prevent overwriting the QR code.
void CompositorHalt();

// Wake the compositor thread immediately (e.g. when a process signals a dirty frame).
void CompositorWake();

// Unregister a process from the compositor (called before the Process is freed).
void CompositorUnregisterProcess(Process* proc);

// Hot-swap the physical framebuffer (called by display driver on mode change).
// Must be called AFTER TtyRemap (uses TtyGetFramebuffer to get the new mapping).
// stridePixels is the row stride in pixels (not bytes).
extern "C" void CompositorRemap(uint64_t fbPhys, uint32_t w, uint32_t h,
                                uint32_t stridePixels);

} // namespace brook
