#pragma once

// arch/aarch64/arch_aarch64.h — AArch64-specific inline implementations of
// the arch abstraction interface (arch/common/arch.h).
//
// *** NOT YET IMPLEMENTED — stubs only. ***
//
// Filling these in requires:
//  1. An AArch64 Clang cross-toolchain (--target=aarch64-elf)
//  2. An AArch64 UEFI bootloader (gnu-efi or EDK2 — UEFI is standard on ARM64)
//  3. Replacing LAPIC/PIC/I8259 with GIC (Generic Interrupt Controller)
//  4. Replacing ACPI MADT LAPIC entries with GICC/GICD entries
//  5. PSCI (Power State Coordination Interface) for CPU on/off/reset
//  6. Replacing GDT/IDT with AArch64 exception vectors (VBAR_EL1)
//  7. No port I/O — all peripherals are MMIO
//
// Page table format is similar (4-level, 4KB pages) but register names differ:
//   TTBR0_EL1 = user-space root (VA [0 .. 2^48-1])
//   TTBR1_EL1 = kernel root    (VA [2^64-2^48 .. 2^64-1])
// PTE bits differ: no x86 NX bit placement; use UXN/PXN for execute control.

#include "../common/arch.h"
#include <stdint.h>

namespace brook::arch {

inline void DisableInterrupts()
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

inline void EnableInterrupts()
{
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

[[noreturn]] inline void HaltForever()
{
    __asm__ volatile("msr daifset, #2");
    for (;;) __asm__ volatile("wfi");
}

inline void MemoryBarrier() { __asm__ volatile("dsb sy"  ::: "memory"); }
inline void StoreBarrier()  { __asm__ volatile("dsb st"  ::: "memory"); }
inline void LoadBarrier()   { __asm__ volatile("dsb ld"  ::: "memory"); }

inline void TlbInvalidatePage(uint64_t virtAddr)
{
    // Invalidate by VA (EL1, inner shareable).
    __asm__ volatile("tlbi vale1is, %0\n\t"
                     "dsb sy\n\t"
                     "isb"
                     : : "r"(virtAddr >> 12) : "memory");
}

inline void TlbFlushAll()
{
    __asm__ volatile("tlbi vmalle1is\n\t"
                     "dsb sy\n\t"
                     "isb" ::: "memory");
}

inline uint64_t ReadPageTableRoot()
{
    uint64_t val;
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(val));
    return val;
}

inline void WritePageTableRoot(uint64_t physAddr)
{
    __asm__ volatile("msr ttbr1_el1, %0\n\t"
                     "isb"
                     : : "r"(physAddr) : "memory");
}

inline void SpinHint() { __asm__ volatile("yield"); }

inline uint32_t CurrentPrivilegeLevel()
{
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return static_cast<uint32_t>(el >> 2) & 0x3;
}

} // namespace brook::arch

// Note: AArch64 has no port I/O.  All x86 drivers that use outb/inb (PCI
// config space, PIC, PIT, PS/2 keyboard) need MMIO equivalents:
//   - PCI: PCIe ECAM (memory-mapped config) replaces CF8/CFC port access
//   - PIC/APIC: GIC (Generic Interrupt Controller) in MMIO
//   - PIT: ARM timer (CNTPCT_EL0/CNTP_CTL_EL0) or SP804 dual timer
//   - PS/2 keyboard: USB HID (no PS/2 on ARM) or PL050 if QEMU virt machine
