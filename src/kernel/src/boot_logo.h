#pragma once

#include <stdint.h>

// Boot logo and progress bar display.
//
// Renders the Brook logo centered on screen with a progress bar below it.
// Called during kernel initialization to show boot progress.
//
// The logo is baked at build time from data/branding/brook-mark.png into a C header.
// The serial log retains stage names; fatal boot errors restore text output.

namespace brook {

// Initialise the boot logo display. Clears the screen to black and renders
// the logo centered horizontally, vertically offset above center.
// Must be called after TtyInit().
void BootLogoInit();

// Update the progress bar (0–100).
// Text is a short status label recorded in the serial log.
void BootLogoProgress(uint8_t percent, const char* text);

// Restore text output and record the boot error.
void BootLogoError(uint8_t percentAtError, const char* errorText);

// Clear the boot logo and restore normal TTY mode.
void BootLogoClear();
// Relinquish framebuffer ownership without clearing the completed desktop.
void BootLogoFinish();
bool BootLogoActive();

} // namespace brook
