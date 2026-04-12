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

} // namespace brook
