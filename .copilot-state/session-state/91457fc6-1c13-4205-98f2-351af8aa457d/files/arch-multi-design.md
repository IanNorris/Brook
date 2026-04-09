# Brook OS — Multi-Architecture Design Notes

## Current state (x86-64 only)

All kernel code lives in `src/kernel/src/` and is implicitly x86-64.
The `src/arch/` directory structure has been created as a skeleton;
no code has been *moved* yet — that will happen incrementally.

---

## Directory structure

```
src/
  arch/
    common/
      arch.h          ← common abstraction interface (port I/O-free)
    x86_64/
      arch_x86_64.h   ← inline impls: CLI/STI, CR3, invlpg, port I/O, MSR
    aarch64/
      arch_aarch64.h  ← inline impls: daifset/clr, TTBR1, TLBI, WFI (stubs)
  kernel/
    src/              ← currently x86-64 kernel; arch-neutral files stay here
```

---

## What needs to change for ARM64

### 1 — Toolchain

New toolchain file: `cmake/toolchains/kernel-aarch64-clang.cmake`
- `--target=aarch64-elf`
- `-mcmodel=large` (or small if kernel fits in 4GB)
- Drop `-mno-red-zone` (not applicable; ARM64 uses a different ABI)
- Keep `-ffreestanding -fno-stack-protector -fno-exceptions -fno-rtti`

### 2 — Boot

UEFI is standard on ARM64 (`-drive if=virtio` + QEMU `virt` machine).
The bootloader (`src/bootloader/`) is compiled `--target=x86_64-unknown-windows`
today — it would need `--target=aarch64-unknown-windows` and its
EFI entry point is portable since gnu-efi uses the same API on both.

BootProtocol (the struct passed from bootloader to kernel) is arch-neutral
already — it only contains physical addresses and memory map entries.

### 3 — Paging / VMM

Page table *structure* is nearly identical (4-level, 4KB pages, 48-bit VA).
Differences:
| | x86-64 | AArch64 |
|---|---|---|
| Root register | CR3 | TTBR0_EL1 / TTBR1_EL1 |
| Kernel VA range | upper half (sign-extend) | TTBR1_EL1 (bits[63:48] = 1) |
| Execute disable | bit 63 (NX) | UXN (bit 54) + PXN (bit 53) |
| Available bits | [9-11], [52-62] | [55-58] (PBHA), [2-4] |
| ASID | — | TTBR0_EL1 bits[63:48] |
| Huge pages | 2MB (PD entry), 1GB (PDPT) | 2MB (level-2), 1GB (level-1) |

`vmm.cpp` page table walks use the x86 PTE layout.  For ARM64 these
need different flag masks — the logic is the same but the constants differ.

Proposed fix: add `src/arch/<arch>/pte.h` with PTE flag constants;
`vmm.cpp` includes the arch-selected one.

### 4 — Exceptions / IDT

x86-64: IDT (256 entries) loaded with `lidt`.
AArch64: exception vector table (VBAR_EL1) — 16 entries × 32 instructions.
These are completely different mechanisms; `idt.cpp` and `gdt.cpp` are
x86-only and would be replaced by `src/arch/aarch64/exception_vectors.S`.

### 5 — Interrupt controller

| x86-64 | AArch64 |
|---|---|
| LAPIC (MMIO) + I/O APIC (MMIO) | GICv2/v3 (MMIO) |
| PIC (8259A, port I/O) | — (no PIC on ARM) |
| IRQ routing via ACPI MADT | ACPI MADT GICC/GICD entries |

`apic.cpp` is x86-only.  A `gic.cpp` would be its ARM equivalent.
ACPICA (the AML bytecode interpreter, planned for later) handles both
since ACPI is also standard on ARM64 server hardware.

### 6 — Port I/O

ARM64 has no `in`/`out` instructions — everything is MMIO.
Files with x86 port I/O that need ARM64 replacements:

| File | x86 mechanism | ARM64 equivalent |
|---|---|---|
| `apic.cpp` | PIC (0x20/0xA0), PIT (0x40-0x43) | GIC MMIO; ARM generic timer |
| `keyboard.cpp` | PS/2 (0x60/0x64) | USB HID (future) or PL050 MMIO |
| `pci.cpp` | CF8/CFC config space | PCIe ECAM (MMIO, addr = base + bus<<20 + dev<<15 + fn<<12 + reg) |
| `virtio_blk.cpp` | legacy virtio BAR0 I/O | modern virtio (BAR0 MMIO) |

The `pci.h` abstraction already isolates callers from the mechanism —
only `pci.cpp` needs an ARM64 implementation.

### 7 — Serial

Current: `serial.cpp` uses COM1 (port 0x3F8, x86).
ARM64 QEMU `virt` machine: PL011 UART at MMIO address `0x09000000`.
`serial.cpp` would need an `#ifdef ARCH_AARCH64` guard or a separate
`src/arch/aarch64/serial_pl011.cpp`.

### 8 — CPU initialisation

`cpu.cpp` enables SSE/FPU via CR0/CR4 and FNINIT.
ARM64: FP/SIMD controlled via CPACR_EL1 bits [21:20] = 0b11 (FPEN).
`cpu.cpp` would be replaced by `src/arch/aarch64/cpu.cpp`.

---

## Recommended migration order

1. **Extract PTE constants** → `src/arch/<arch>/pte.h` (unblocks ARM64 VMM)
2. **Wrap port I/O** → replace inline `outb`/`inb` in each driver with
   `brook::arch::IoOutB` / `brook::arch::IoInB` from `arch_x86_64.h`
   (makes the callsites visibly arch-specific)
3. **Move GDT/IDT/APIC** → `src/arch/x86_64/` (they have no ARM64 counterpart)
4. **Implement AArch64 stubs** → GIC, exception vectors, PL011 serial, CPU init
5. **QEMU virt machine target** → add `-machine virt -cpu cortex-a57` run script

---

## Files that are already arch-neutral (keep as-is)

- `pmm.cpp/h` — bitmap allocator, no arch assumptions
- `heap.cpp/h` — slab allocator, no arch assumptions
- `device.h/cpp` — device registry, no arch assumptions
- `ramdisk.h/cpp` — memory-backed block device
- `vfs.h/cpp` — VFS layer (FatFS is arch-neutral C)
- `fatfs_glue.h/cpp` — FatFS bridge
- `tty.h/cpp` — framebuffer TTY (framebuffer address in BootProtocol)
- `kprintf.h/cpp` — formatted output layer
- `boot_protocol/boot_protocol.h` — all physical addresses, no arch bits

## Files that are entirely x86-64-specific

- `gdt.h/cpp` — no ARM64 GDT
- `idt.cpp` — replaced by VBAR_EL1 on ARM64
- `apic.h/cpp` — replaced by GIC
- `cpu.cpp` — CR0/CR4/SSE; replaced by CPACR_EL1
- `keyboard.h/cpp` — PS/2 port I/O; replaced by USB HID or PL050
- `pci.cpp` — CF8/CFC; need PCIe ECAM version for ARM64
- `virtio_blk.cpp` — legacy virtio I/O BAR; need MMIO virtio for ARM64
