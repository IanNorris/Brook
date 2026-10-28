Brook portable QEMU candidate
============================
This is a private validation candidate, not yet cleared for public binary
redistribution. See manifest.json and the corresponding-source review report.
No existing user disk, account, Steam client, paid game, save or Quake data is
included. QEMU itself is installed on the host; its binaries are not bundled.

Host requirements
-----------------
Python 3.10 or newer; QEMU 8 or newer including qemu-system-x86_64 and qemu-img
on PATH. About 8 GiB guest RAM plus host headroom is recommended. The base disk
has 16 GiB virtual capacity; writable instance files grow with use.

Linux: install your distribution's qemu-system-x86/qemu, qemu-img/qemu-utils
and python3 packages. On NixOS: nix-shell -p qemu python3. KVM access and a free
hardware virtualization backend are needed for accelerated execution.

macOS: install Python and QEMU, e.g. brew install python qemu.
Intel Macs use HVF when available. Apple Silicon must emulate the x86-64 CPU
with TCG and will be substantially slower. No ARM Brook guest is included.

Windows x64: install Python 3 (with the py launcher) and QEMU from a trusted
distributor linked at https://www.qemu.org/download/#windows ; add QEMU to
PATH. Enable Windows Hypervisor Platform for WHPX; reboot if required.
Windows ARM64 and macOS/Windows native execution have not been validated.

Launch
------
Linux:   sh run-linux.sh
macOS:   sh run-macos.command
Windows: run-windows.cmd

Options: --instance demo --memory 8192 --cpus 8 --accel auto
         --audio pipewire  (Linux; alternatives pa or sdl if QEMU supports it)
         --audio coreaudio (macOS; untested here)
         --audio dsound    (Windows; untested here)
         --display none   (headless acceptance/profiling)
         --accel tcg      (slow software CPU fallback, default 2 vCPUs)

Audio defaults to silent. Graphics are software rendered and work without
VirGL/Venus, host OpenGL passthrough or proprietary host GPU libraries. This
is a portability baseline, not the accelerated gaming benchmark image.

State persists under ~/.local/share/BrookVM/INSTANCE (Linux/macOS) or
%LOCALAPPDATA%\BrookVM\INSTANCE (Windows). Each instance has its own overlay
and UEFI variables; the shipped base is never writable. Keep the bundle at
the same path after creating an instance. To use another build/location,
choose a new instance name. Do not delete state while it is running.

Use the Brook launcher Shutdown/Restart items or the MCP lifecycle tools.
Closing the QEMU window is a forced stop and may lose recent guest writes.
The host QEMU mouse-release shortcut is normally Ctrl+Alt; check its menu.

MCP / VM harness
----------------
The runner prints the absolute path to session.json. Configure a stdio MCP
server with command python3 (or Python's Windows absolute path) and arguments
["/absolute/bundle/mcp.py", "/absolute/instance/session.json"].
This adapter offers status, screenshots as paths, input, log tail, sync,
clean shutdown/restart and an explicitly acknowledged force-stop. First call
vm_status, then pass its generation to other tools. It never controls the host
desktop. The private session descriptor contains a local control token: do not
publish it. Screenshot artifacts are PPM files; most image tools can read them.

linux-development-bridge contains the full existing Brook MCP implementation
(guest jobs, profiles, stats, crops, UI and audio diagnostics). It depends on
the repository's development runner/registry and is not a drop-in Windows or
macOS MCP server. The portable subset is deliberately labelled separately.
Guest debug networking is off unless --bridge-port PORT is explicitly set;
that legacy guest protocol is unauthenticated and bound to host loopback.

Software
--------
See software.json for tested scope versus distribution route. The base has
Chromium, GIMP, Mousepad, VLC, OpenTTD with open base assets, Qalculate!, Vim,
TCC, Python, Git, coreutils and Brook's native tools. Some applications remain
experimental or slow. Chromium's sandbox is disabled by the compatibility
launcher: this is an experimental OS image, not a hardened daily browser.

installers contains opt-in publisher-download helpers, not freeware payloads.
Those downloads change the instance's redistribution status. Do not publish
an instance overlay after installing proprietary software or signing in.
