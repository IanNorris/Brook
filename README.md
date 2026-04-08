# Brook OS

A hobby x86_64 operating system focused on code quality and clean architecture.

## Goals

- Minimal microkernel with drivers in userspace
- Ring separation established early
- Linux ABI compatibility
- Windows ABI compatibility (clean-room, bring your own DLLs)
- Unified Clang/LLVM toolchain throughout

## Architecture

```
Bootloader (UEFI PE/COFF)
  └─ Loads kernel ELF at high virtual addresses
  └─ Passes boot protocol (memory map, framebuffer) to kernel

Kernel (ring 0, minimal)
  └─ Memory management
  └─ IPC / message passing
  └─ Two-tier scheduler (kernel allocates quanta, userspace picks policy)

Drivers (ring 3, userspace processes)
  └─ Storage, filesystem, display, input

Apps
  └─ Shell, ...
```

## Building

### Prerequisites

- `clang` / `clang++`
- `lld` (`lld-link` and `ld.lld`)
- `cmake` ≥ 3.25
- `ninja`
- `qemu-system-x86_64` + OVMF (for running)

On NixOS:
```bash
nix-shell -p clang lld cmake ninja qemu OVMF
```

### Build

```bash
./scripts/build.sh          # Debug build (default)
./scripts/build.sh Release  # Release build
```

### Run in QEMU

```bash
./scripts/run-qemu.sh
```

## Project Structure

```
src/
  bootloader/       UEFI bootloader (PE/COFF via Clang)
  kernel/           Kernel (bare metal ELF, high virtual addresses)
  drivers/          Userspace driver processes
  apps/             User applications (shell, ...)
  shared/
    boot_protocol/  Bootloader↔kernel handoff structures
    inc_km/         Kernel-mode shared headers
    src_km/         Kernel-mode shared implementation
    inc_um/         Usermode shared headers
    src_um/         Usermode shared implementation
cmake/
  toolchains/       Per-target Clang toolchain files
vendor/
  uefi-headers/     UEFI header-only library (submodule)
scripts/
  build.sh          Build orchestration
  run-qemu.sh       QEMU test runner
```

## Legal

See [LEGAL.md](LEGAL.md) for notes on clean-room Windows ABI implementation.
