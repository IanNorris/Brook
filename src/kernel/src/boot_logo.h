#pragma once

#include <stdint.h>

// Boot logo and progress bar display.
//
// Renders the Brook logo centered on screen with a progress bar below it.
// Called during kernel initialization to show boot progress.
//
// The logo is baked at build time from data/brook_logo.png into a C header.
// Progress bar shows percentage and becomes an error code display on failure.

namespace brook {

// Initialise the boot logo display. Clears the screen to black and renders
// the logo centered horizontally, vertically offset above center.
// Must be called after TtyInit().
void BootLogoInit();

// Update the progress bar (0–100).
// Text is a short status label shown next to the percentage.
void BootLogoProgress(uint8_t percent, const char* text);

// Show an error on the progress bar (red bar, error text).
void BootLogoError(uint8_t percentAtError, const char* errorText);

// Clear the boot logo and restore normal TTY mode.
void BootLogoClear();

} // namespace brook
