#pragma once

#include <stdint.h>

// Brook kernel shell — boot script executor and interactive command line.
//
// The shell reads a simple line-based script format:
//   - Lines starting with '#' are comments
//   - Empty lines are ignored
//   - "run <path> [args...]" spawns a user-mode ELF process
//   - "wait" blocks until all running user processes exit
//   - "set <key> <value>" sets a shell variable (grid, scale, etc.)
//   - "clear" clears the TTY
//   - "ps" lists running processes
//   - "help" shows available commands
//
// Boot sequence:
//   1. ShellExecScript("/boot/INIT.RC") runs the boot script
//   2. ShellInteractive() enters the interactive prompt
//
// Process placement:
//   The shell auto-tiles processes in a grid on the compositor.
//   "set grid <cols>x<rows>" overrides the default layout.
//   "set scale <n>" sets the compositor downscale factor.

namespace brook {

// Execute a boot script from a VFS path.
// Returns 0 on success, -1 if the file could not be opened.
int ShellExecScript(const char* path);

// Enter the interactive shell.  Reads from the keyboard, executes commands.
// Never returns (runs until shutdown/reboot).
[[noreturn]] void ShellInteractive();

// Default environment block passed to every user-mode process spawned by
// the shell.  Other in-kernel spawners (e.g. terminal.cpp launching bash)
// MUST extend this rather than build their own — otherwise SSL_CERT_FILE,
// WAYLAND_DISPLAY, XDG_*, GTK_*, FONTCONFIG_*, etc. silently disappear
// from descendant processes' environments.
//
// Returns a NULL-terminated array of "KEY=VALUE" strings.
const char* const* ShellGetDefaultEnvp();

// Number of entries (excluding the terminating nullptr) in
// ShellGetDefaultEnvp().
uint32_t ShellGetDefaultEnvpCount();

} // namespace brook
