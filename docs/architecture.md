# How Brook is put together

Brook is a from-scratch x86-64 operating system. This page is a map of its
main parts and how they fit together. The code is the final authority; each
section names where to look.

## Boot

A small UEFI bootloader (`src/bootloader`) loads the kernel ELF and an initial
RAM disk, collects the memory map, framebuffer and ACPI tables, and hands
over through the boot protocol in `src/shared/boot_protocol`. On the laptop,
boot *generations* keep a known-good kernel alongside the new one, and a
guarded slot rolls back automatically if a new kernel fails to reach the
desktop.

## Kernel

The kernel (`src/kernel`) is a preemptive, SMP, 64-bit kernel written in C++:

- **Memory:** 4-level paging, demand paging and copy-on-write, shared and
  file-backed mappings, memory pressure handling and controlled
  out-of-memory recovery.
- **Processes:** POSIX processes and threads, signals, futexes (including
  priority-inheritance and robust futexes), user and mount namespaces, and a
  per-CPU scheduler. Scheduling policies are loadable modules
  (`src/drivers/sched_*`).
- **Files:** a VFS with FAT, ext2/ext4, tmpfs, procfs and pipes, plus a page
  cache.
- **Networking:** Brook's own TCP/IP stack with DHCP and DNS.
- **Linux compatibility:** a Linux-compatible system-call layer, enough to run
  glibc and musl programs unmodified, including browsers, Electron apps and
  Steam's runtime container.

Drivers are loadable kernel modules (`src/drivers`): virtio devices, PS/2,
xHCI USB, Intel HDA audio and display drivers. The Intel Broadwell GPU driver
and the laptop's AHCI storage live in the kernel.

## Graphics and the desktop

The desktop has several cooperating parts:

| Part | Where | Role |
|---|---|---|
| Window manager | kernel (`window.cpp`, `compositor.cpp`) | Windows, focus, input routing, the scene description |
| Desktop renderer | user mode (`tools/vulkan-compositor-pkg`) | Draws the scene on the GPU with Vulkan, including the glass blur, and presents it |
| Clearwater shell | user mode (`tools/clearwater-shell-pkg`) | The launcher, taskbar, flyouts and notifications, supervised and restartable |
| Wayland server | user mode (`tools/waylandd-pkg`) | Speaks the Wayland protocol to apps and hosts rootless Xwayland for X11 apps |

The kernel owns the display: a crashed renderer or shell is restarted without
taking the desktop down, and the kernel falls back to drawing the desktop
itself. Apps hand their buffers to the Wayland server as shared memory or as
GPU (DMA-BUF) buffers. Brook's own syscalls carry those buffers and the scene
to the renderer.

The visual design (materials, modes, accent colours, geometry) is in
[the Clearwater design](design/clearwater-next/DESIGN.md).

## User space and packages

User programs come from Nix: `/nix/store` is laid out as on NixOS, and
`/nix/profile` is the installed profile. The App Gallery and `nix-install`
fetch closures from the NixOS binary cache. Brook-specific packages (Settings,
the shell, the Wayland server, patched Mesa) are Nix expressions under
`tools/*-pkg` and `nix/`. User data lives in `/data/home`.

## Tooling

- **Tests:** about 390 host-native test suites, pixel-exact "golden" images of
  the desktop chrome, bare-metal test programs run in QEMU, and a machine-code
  audit of kernel entry and exit paths.
- **Debug bridge:** `tools/vm_bridge` is an MCP server and command-line tool
  for inspecting and driving a running Brook system: screenshots, input, logs,
  command jobs, file transfer and guarded deploys to real hardware. See its
  [orientation](../tools/vm_bridge/ORIENTATION.md).
- **Panics:** a kernel panic draws QR codes carrying the registers and stack
  trace, and a panic journal survives the reboot.
