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
