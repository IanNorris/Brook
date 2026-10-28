Brook portable
==============
Install Python 3.10+ and QEMU, extract this folder, then launch:

Windows: double-click run-windows.cmd
Linux:   sh run-linux.sh
macOS:   sh run-macos.command (untested)

QEMU downloads: https://www.qemu.org/download/
Windows downloads: https://www.qemu.org/download/#windows
The Windows launcher also checks %ProgramFiles%\qemu.
On Windows enable Windows Hypervisor Platform for CPU acceleration.
Apple Silicon uses slow x86 software emulation; no ARM guest is included.

brook.qcow2 is your writable disk. Apps, settings and files persist directly
in it. There are no instance names, overlays, AppData folders or checksums to
manage. Move the whole folder when the VM is off; copy it to make a backup.
The two OVMF files are required UEFI firmware; OVMF_VARS.fd also stores changes.
Do not overwrite your used disk with a fresh release if you want to keep it.

Audio is on by default. Graphics use software rendering for portability;
WHPX/KVM accelerate the CPU, not the guest GPU.
Use Brook's launcher Shutdown/Restart menu. Closing QEMU forces power off.
Startup diagnostics are written to qemu.log and guest output to serial.log
beside the launcher. No files are placed in AppData or your home directory.

Optional command-line switches:
    --memory 8192 --cpus 8
    --audio none
    --display gtk
    --accel tcg  (slow CPU-emulation fallback)
Some QEMU/SDL builds mis-scale mouse input after resizing; prefer GTK or
restore SDL's native window size with Ctrl+Alt+0.

Optional developer/MCP files are distributed separately. --mcp enables a local
control endpoint and temporary session.json for the optional mcp.py adapter.
Guest debug networking is opt-in with --bridge-port PORT (host loopback only).
Ordinary launches do not start the MCP endpoint or create a session descriptor.

GNOME Commander file operations remain experimental. Chromium audio has a
controlled Linux pass; Windows audio still needs confirmation. Chromium's
compatibility launcher disables its sandbox. This is an experimental OS.
This private validation candidate awaits component notice/source review before
public binary release. No paid games, Quake data/music, accounts or saves are
included. Optional publisher installers are not bundled with this minimal ZIP.
