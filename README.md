<p align="center">
  <img src="docs/images/brook_logo.png" alt="Brook OS" width="600">
</p>

<h3 align="center">A hobby x86-64 operating system built from scratch</h3>

<p align="center">
  UEFI · C++ · Clang/LLVM · SMP · Window Manager · Runs DOOM
</p>

---

<p align="center">
  <img src="docs/images/brook_wm.png" alt="Brook OS window manager running DOOM and a bash terminal" width="800">
</p>

<p align="center"><em>Brook's compositing window manager running DOOM alongside an interactive bash terminal</em></p>

<p align="center">
  <img src="docs/images/brook_quake2_lan.png" alt="Two Brook VMs playing Quake 2 LAN deathmatch over a VDE virtual switch" width="800">
</p>

<p align="center"><em>Two Brook VMs playing Quake 2 LAN deathmatch over a VDE virtual switch — real UDP sockets, static-IP config, multi-NIC kernel routing</em></p>

## What is Brook?

Brook is a hobby operating system for x86-64, written from scratch in C++ with
a unified Clang/LLVM toolchain. It boots via UEFI, runs on multiple CPU cores,
has a compositing window manager with window chrome, and implements enough of
the Linux syscall ABI to run unmodified Linux binaries — including bash, DOOM,
and a C compiler.

This is my second OS project (the first being
[Enkel](https://github.com/IanNorris/Enkel)). Where Enkel was about getting
things working, Brook is about doing it **right** — clean architecture,
modular drivers, and a codebase that's a pleasure to work in.

## Confirmed Working Software

These run as unmodified Linux ELF binaries on Brook, linked against musl libc:

| Software | Version | Status | Notes |
|----------|---------|--------|-------|
| [bash](https://www.gnu.org/software/bash/) | 5.2 | ✅ Working | Interactive shell with readline, job control, scripting |
| [DOOM](https://github.com/ozkl/doomgeneric) | 1.9 | ✅ Working | Runs in a WM window or full-screen, with keyboard input |
| [busybox](https://busybox.net/) | 1.36 | ✅ Working | ls, cat, echo, wc, head, tail, grep, and many more |
| [TCC](https://bellard.org/tcc/) | 0.9.27 | ✅ Working | Compiles and runs C programs natively on Brook |
| [CoreMark](https://www.eembc.org/coremark/) | 1.0 | ✅ Working | CPU benchmark, runs to completion |
| [musl libc](https://musl.libc.org/) | 1.2 | ✅ Working | Standard C library, dynamically linked |

## Features

### Kernel
- **UEFI bootloader** — custom bootloader loads ELF kernel at high virtual addresses
- **SMP** — symmetric multiprocessing with per-CPU run queues and load balancing
- **MLFQ scheduler** — multi-level feedback queue loaded as a kernel module, with configurable policy
- **Virtual memory** — 4-level paging, per-PID ownership tracking, guard pages
- **Kernel heap** — kmalloc/kfree with slab-style allocation
- **Loadable kernel modules** — drivers compiled separately and loaded from disk at boot
- **VFS + FAT filesystem** — virtio-blk backed storage with full read/write support

### Linux Compatibility
- **~80 syscalls** — open, read, write, mmap, fork, execve, pipe, dup2, ioctl, poll, and more
- **ELF loader** — loads standard Linux ELF binaries with dynamic linking (musl ld.so)
- **Signals** — SIGINT, SIGQUIT, SIGTSTP, SIGCONT, SIGKILL, SIGPIPE, SIGCHLD with rt_sigaction
- **Pipes** — anonymous pipes with blocking read/write and proper EOF semantics
- **fork/exec** — full process creation with address space cloning

### Window Manager
- **Compositing WM** — desktop wallpaper, window chrome with title bars, z-ordered rendering
- **Terminal emulator** — VT100/ANSI escape sequences, connects to bash via pipe pair
- **Mouse + keyboard** — cursor rendering, click-to-focus, Ctrl+C/Z/\ signal keys
- **Per-process framebuffers** — each window renders to its own virtual framebuffer
- **Upscaling** — configurable per-window scale factor (DOOM renders at 4× to fill the screen)

### Drivers (loadable modules)
| Module | Description |
|--------|-------------|
| `bochs_display` | BGA/bochs VBE display driver (1920×1080) |
| `ps2_kbd` | PS/2 keyboard — Shift, Ctrl, Alt, CapsLock |
| `ps2_mouse` | PS/2 mouse driver |
| `virtio_blk` | Virtio block device for disk I/O |
| `virtio_input` | Virtio input tablet for absolute mouse positioning |
| `sched_mlfq` | Multi-level feedback queue scheduling policy |

## Building

### Prerequisites

- Clang/LLVM 18+, LLD, NASM
- CMake ≥ 3.25, Ninja
- Python 3 with `freetype-py` (for font atlas generation)
- QEMU + OVMF (for running)

On NixOS / with Nix:
```bash
nix-shell -p clang_18 lld_18 llvm_18 cmake ninja python3 python3Packages.freetype-py nasm
```

### Build

```bash
./scripts/build.sh          # Debug build
./scripts/build.sh Release  # Release build
```

### Run in QEMU

```bash
./scripts/run-qemu.sh       # Launches QEMU with OVMF firmware
```

Boot scripts in `data/scripts/` control what runs at startup:

| Script | Description |
|--------|-------------|
| `wm.rc` | Window manager with bash terminal |
| `wmdoom.rc` | Window manager with DOOM + bash terminal |
| `doomfs.rc` | Full-screen DOOM (no WM) |
| `shell.rc` | Direct serial shell (no graphics) |

## Project Structure

```
src/
  bootloader/       UEFI bootloader (PE/COFF, loads kernel ELF)
  kernel/           Kernel (scheduler, VMM, VFS, syscalls, compositor)
  drivers/          Loadable kernel modules (display, input, block, scheduler)
  apps/             Userspace programs (hello, mandelbrot, coremark, ...)
  shared/
    boot_protocol/  Bootloader ↔ kernel handoff structures
    inc_km/         Kernel-mode shared headers
    src_km/         Kernel-mode shared sources
    inc_um/         Usermode shared headers
    src_um/         Usermode shared sources
cmake/
  toolchains/       Clang cross-compilation toolchain files
vendor/
  uefi-headers/     UEFI specification headers (submodule)
scripts/            Build, run, and asset conversion scripts
data/
  scripts/          Boot configuration scripts (.rc files)
docs/
  images/           Screenshots and logo
```

## Acknowledgements

- [DOOM Generic](https://github.com/ozkl/doomgeneric) — portable DOOM engine
- [musl libc](https://musl.libc.org/) — C standard library
- [bash](https://www.gnu.org/software/bash/) — Bourne Again Shell
- [busybox](https://busybox.net/) — Unix utilities in a single binary
- [TCC](https://bellard.org/tcc/) — Tiny C Compiler
- [Hack](https://sourcefoundry.org/hack/) — terminal typeface

## License

MIT — see [LICENSE](LICENSE).
