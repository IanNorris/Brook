# Bootloader & Shared Library Documentation

## Overview

Brook's UEFI bootloader loads the kernel ELF, sets up page tables, and
hands off to the kernel via the BootProtocol structure. Shared code
provides serial output, crash dumps, and common definitions.

## Bootloader Files

| File | Lines | Purpose |
|------|-------|---------|
| main.cpp | ~140 | EFI_MAIN entry, boot sequence orchestration |
| elf_loader.cpp | ~170 | Kernel ELF loading (PT_LOAD segments) |
| paging.cpp | ~140 | Identity map + kernel high map page tables |
| memory.cpp | ~150 | UEFI memory map collection, ExitBootServices |
| graphics.cpp | ~110 | GOP initialization, mode selection |
| fs.cpp | ~85 | ESP file reading via Simple File System Protocol |
| config.cpp | ~90 | BROOK.CFG parsing (key=value) |
| console.cpp | ~70 | UEFI Simple Text Output (pre-ExitBootServices) |
| acpi.cpp | ~50 | ACPI 2.0 RSDP location from config tables |

## Shared Files

| File | Lines | Purpose |
|------|-------|---------|
| serial.cpp | ~80 | COM1 serial output with per-CPU spinlock |
| crash_dump.c | ~240 | User-mode crash dump writer (stack walk + registers) |
| boot_protocol.h | ~120 | BootProtocol ABI between bootloader and kernel |
| mem_tag.h | ~30 | 3-bit memory region tagging |
| elf.h | ~60 | ELF header/program header definitions |

## Boot Sequence

```
EFI_MAIN
  → Console init (UEFI text output)
  → LoadConfig (BROOK.CFG from ESP)
  → GopInit (find best resolution: 1920x1080 > 1280x720 > largest)
  → FindAcpiRsdp (scan UEFI config tables for ACPI 2.0 GUID)
  → LoadKernel (ELF PT_LOAD to physical 0x400000+, virtual 0xFFFFFFFF80000000+)
  → LoadInitrd (optional ramdisk image)
  → CollectMemoryMap + ExitBootServices
  → BuildPageTables (identity 0-128GB via 2MB, kernel via 4KB)
  → LoadCR3, jump to kernel entry via inline asm
```

## Key Design Decisions

- **Identity map**: All physical RAM 0-128GB mapped via 2MB pages for
  simplicity. Kernel refines later based on actual memory map.
- **Kernel address**: Physical 0x400000 (preferred), virtual 0xFFFFFFFF80000000.
  Falls back to AllocateAnyPages if preferred address is unavailable.
- **BootProtocol**: Passed in RDI (SysV ABI). Contains memory map pointer,
  framebuffer info, ACPI RSDP, initrd address/size.
- **Serial**: Per-CPU spinlock with ISR re-entry protection. Safe for SMP.

## Audit Findings (2026-05-10)

### Known Issues
- **BRO-127**: Crash dump stack walker dereferences unvalidated user pointers
- ELF entry point not validated to be within kernel image bounds
- Graphics mode query leaks EFI pool memory (freed at ExitBootServices anyway)
- Config parser doesn't handle Mac-only line endings (\\r without \\n)
- ACPI RSDP pointer from UEFI config table not signature-validated

### Overall Assessment
**Good for a hobby UEFI bootloader.** Follows standard UEFI patterns
correctly. The identity-map-all-RAM approach is simple and intentional.
Main risks are in the crash dump user-pointer dereference and missing
ELF entry validation — both are defense-in-depth concerns rather than
likely failure modes.

## Boot timing handoff (version-5 extension)

The loader records raw boot-CPU TSC values in `BootProtocol::timing`
(`src/shared/boot_protocol/boot_protocol.h`): EfiMain entry, F5-wait start
and end, just before ExitBootServices and just before the kernel jump, plus
flags, the configured wait and the keystrokes ConIn returned during the wait.

The handoff **stays at version 5**. Every kernel already installed on the Dell
(both update slots and the RECOVERY/RECOV2 pairs) rejects other versions: the
EFI runtime inventory accepts only 3..5 and EFI power-off required exactly 5,
so a version bump would silently disable EFI power-off (and with it the
AC-loss and USB-boot power-off paths) on every older kernel. Instead the loader
sets `extensions` (the former always-zero `efiRuntimeMtrrReserved`) to
`BootExtensionTimingMagic` and appends `timing` after the version-5 fields.
Older loaders zero-initialise the struct, so `extensions` is 0 there.

The kernel reads `timing` only when `BootHasTiming()` sees the marker, so a new
kernel booted by an older loader shows "n/a" for firmware, menu and loader and
never reads past the version-5 fields (`BootProtocolV5Size`); the EFI power-off
copy is also limited to that size. An older kernel ignores the appended fields.
Future additions should follow the same pattern (another marker bit or field
announced by the loader), not a version bump, until every installed kernel
accepts newer versions.

The kernel (`src/kernel/src/boot_timing.*`) converts with the TSC rate it
measures during LAPIC calibration and logs one `BOOT_TIMING` line plus a
`BOOT_TIMING_DETAIL` line when the desktop is first presented:

- **firmware:** ACPI FPDT `OsLoaderStartImageStart` when an FBPT is present
  (`src=fpdt`), else loader-entry TSC since reset (`src=tsc`, valid only below
  ten minutes, i.e. after a clean reset). OVMF publishes no FPDT.
- **wait:** the measured F5 wait (0 when `RECOVERY_MENU` is off).
- **loader:** loader entry to kernel entry, minus the wait. In the guarded
  loader this includes the guard-record check, the optional data-disk
  inspection and reading/verifying the payload.
- **kernel:** kernel entry to the first presented desktop frame. In a guarded
  desktop kernel this includes the in-kernel guard checker and desktop handoff.
  After an automatic repair, the recovery kernel resets the machine; the TSC
  restarts, so every number describes only the final boot.

## Recovery wait

`RECOVERY_MENU=1` enables the F5 prompt; `RECOVERY_MENU_MS` (0–10000,
default 500) sets its length, polled every 10 ms. In QEMU/OVMF a PS/2 F5 sent
during the prompt selects recovery. On the Dell the wait reportedly ignores
input: unconfirmed, but the likely cause is that a fast-boot firmware path does
not connect the keyboard to ConIn for the default boot entry (F5 worked after a
firmware-menu selection). `BOOT_TIMING_DETAIL wait_keys=` / `last_key=` now
show whether ConIn delivered any key during the wait; if it reports zero while
keys are pressed, connecting the console controllers before the wait is the
next step (not done: it needs physical testing).
