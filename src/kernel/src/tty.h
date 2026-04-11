#pragma once

#include <stdint.h>
#include "boot_protocol/boot_protocol.h"

// Kernel TTY — renders text to the UEFI framebuffer using a pre-baked glyph atlas.
//
// Features:
//   - Hack 16px anti-aliased glyphs (baked at build time, no FPU needed at runtime)
//   - Automatic word-wrap and newline handling
//   - Scrolling: when the cursor hits the bottom, all rows scroll up by one line
//   - Configurable foreground/background colour
//
// Lifecycle:
//   TtyInit() must be called after VmmInit() + HeapInit() + PmmEnableTracking().
//   After that, TtyPrintf/TtyPuts/TtyPutChar are safe to call from any kernel context
//   that is NOT inside an interrupt handler (framebuffer writes are not re-entrant).

namespace brook {

// Initialise the TTY: maps the framebuffer into virtual address space,
// clears it to the default background colour, and resets the cursor.
// Returns true on success; false if the VMM cannot map the framebuffer.
bool TtyInit(const Framebuffer& fb);

// Output a single character.  Handles \n, \r, \t.  Word-wraps at screen edge.
void TtyPutChar(char c);

// Output a null-terminated string.
void TtyPuts(const char* str);

// printf-style formatted output.  Supports the same specifiers as SerialPrintf:
// %s %d %u %x %lu %lx %ld %p %c %%  (no width/precision modifiers except %p)
void TtyPrintf(const char* fmt, ...);
void TtyVPrintf(const char* fmt, __builtin_va_list args);

// Set foreground and background colours (0x00RRGGBB — top byte ignored).
void TtySetColors(uint32_t fg, uint32_t bg);

// Move cursor to the top-left and clear the screen.
void TtyClear();

// Constrain TTY output to a rectangular region of the framebuffer.
// All rendering and scrolling is limited to this region.  Passing all-zero
// resets to the full framebuffer.  The region is specified in pixels.
void TtySetRegion(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// Return true if TtyInit() has completed successfully.
bool TtyReady();

// Suppress framebuffer rendering.  While suppressed, TtyPutChar/TtyPuts/TtyPrintf
// still output to the serial console but skip all framebuffer writes.
// Used during boot-logo display to prevent TTY from overwriting the logo.
void TtySuppressDisplay(bool suppress);

// Retrieve framebuffer info for direct rendering (e.g. QR panic screen).
// Returns false if TTY is not initialised.
bool TtyGetFramebuffer(uint32_t** outPixels, uint32_t* outWidth,
                       uint32_t* outHeight, uint32_t* outStride);

// Retrieve physical framebuffer info for user-mode mmap.
bool TtyGetFramebufferPhys(uint64_t* outPhysBase, uint32_t* outWidth,
                           uint32_t* outHeight, uint32_t* outStride);

} // namespace brook
