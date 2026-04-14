<p align="center">
  <img src="docs/images/brook_logo.png" alt="Brook OS" width="600">
</p>

<h3 align="center">A hobby x86-64 operating system built from scratch</h3>

<p align="center">
  UEFI · Clang/LLVM · SMP · Window Manager · Runs DOOM
</p>

---

![Brook OS window manager running DOOM and a terminal](docs/images/brook_wm.png)

## What is Brook?

Brook is a hobby operating system for x86-64, written in C++ with a unified
Clang/LLVM toolchain. It boots via UEFI, runs on multiple cores, has a
compositing window manager, and can run DOOM.

This is my second OS project (the first being
[Enkel](https://github.com/IanNorris/Enkel)). Where Enkel focused on getting
things working, Brook focuses on **code quality** and **clean architecture**.

## Features

### Kernel
- **UEFI bootloader** — loads ELF kernel at high virtual addresses (0xFFFFFFFF80000000)
- **SMP** — symmetric multiprocessing with per-CPU scheduling
- **MLFQ scheduler** — multi-level feedback queue, loaded as a kernel module
- **Virtual memory** — 4-level page tables, per-PID ownership tracking, demand paging
- **Kernel heap** — kmalloc/kfree with slab-style allocation
- **Loadable kernel modules** — drivers loaded from disk at boot
- **VFS + FAT filesystem** — virtio-blk backed storage with full read/write

### Userspace
- **Linux syscall ABI** — enough of the Linux ABI to run real ELF binaries
- **musl libc** — standard C library for userspace programs
- **bash** — full interactive bash shell with readline
- **busybox** — coreutils (ls, cat, echo, etc.)
- **TCC** — Tiny C Compiler runs natively, can compile and execute C programs
- **DOOM** — the classic id Software game, running in a window
- **Signals** — SIGINT, SIGQUIT, SIGKILL, SIGPIPE, SIGCHLD, with rt_sigaction
- **Pipes** — anonymous pipes with proper blocking read/write semantics
- **fork/exec** — full process creation with copy-on-write

### Window Manager
- **Compositing WM** — wallpaper, window chrome, title bars, z-ordering
- **Terminal emulator** — VT100/ANSI escape sequences, connects to bash via pipes
- **Mouse support** — cursor rendering, window focus by click
- **Per-process framebuffers** — each window gets its own virtual framebuffer
- **Upscaling** — configurable per-window scale factor (DOOM renders at 4×)

### Drivers (loadable modules)
- `bochs_display` — BGA/bochs VBE display driver
- `ps2_kbd` — PS/2 keyboard with Shift, Ctrl, Alt, CapsLock
- `ps2_mouse` — PS/2 mouse driver
- `virtio_blk` — virtio block device for disk I/O
- `virtio_input` — virtio input (tablet) for absolute mouse positioning
- `sched_mlfq` — multi-level feedback queue scheduling policy

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
- `wm.rc` — Window manager with terminal
- `wmdoom.rc` — Window manager with DOOM + terminal
- `doomfs.rc` — Full-screen DOOM
- `shell.rc` — Direct serial shell (no WM)

## Project Structure

```
src/
  bootloader/       UEFI bootloader (PE/COFF)
  kernel/           Kernel core (memory, scheduler, VFS, syscalls, WM)
  drivers/          Loadable kernel modules
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
scripts/
  build.sh          Build orchestration
  run-qemu.sh       QEMU launcher
  convert_wallpaper.py   Wallpaper image converter
  generate_font_atlas.py Font atlas generator
data/
  scripts/          Boot scripts (.rc files)
  wallpaper.raw     Desktop wallpaper (raw XRGB)
docs/
  images/           Screenshots and logo
```

## Acknowledgements

- [DOOM Generic](https://github.com/ozkl/doomgeneric) — portable DOOM port
- [musl libc](https://musl.libc.org/) — C standard library
- [bash](https://www.gnu.org/software/bash/) — shell
- [busybox](https://busybox.net/) — coreutils
- [TCC](https://bellard.org/tcc/) — Tiny C Compiler
- [Hack font](https://sourcefoundry.org/hack/) — terminal font

## Legal

See [LEGAL.md](LEGAL.md) for notes on the clean-room Windows ABI compatibility layer.

## License

MIT — see [LICENSE](LICENSE).
