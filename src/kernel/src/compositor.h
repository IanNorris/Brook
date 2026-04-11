#pragma once

#include <stdint.h>

namespace brook {

struct Process;

// Initialise the compositor (caches physical FB info).
// Must be called after TtyInit().
void CompositorInit();

// Allocate a per-process virtual framebuffer that is the same size as
// the physical FB. The process sees a full-resolution screen; the
// compositor downscales it by `scale`:1 and places it at (destX, destY)
// on the physical framebuffer.
// scale=0 means no compositing (direct physical FB mapping, legacy mode).
bool CompositorSetupProcess(Process* proc, int16_t destX, int16_t destY, uint8_t scale);

// Composite all active virtual framebuffers onto the physical framebuffer.
// Called from the timer tick at a configurable interval.
void CompositorTick();

} // namespace brook
