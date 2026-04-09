#pragma once

// arch/common/arch.h — Architecture abstraction interface for Brook OS.
//
// Every architecture port must implement the functions declared here.
// The actual implementations live in arch/<arch>/ and are compiled in
// as the sole arch-specific translation units.
//
// This header is intentionally minimal — only things that are genuinely
// shared across >1 architecture belong here.  x86-specific concepts
// (GDT, IDT, LAPIC, etc.) stay in src/kernel/src/ and are compiled only
// for x86_64.

#include <stdint.h>

namespace brook::arch {

// ---- Interrupt control ----

// Disable interrupts (x86: cli / ARM64: msr daifset, #2).
void DisableInterrupts();

// Enable interrupts (x86: sti / ARM64: msr daifclr, #2).
void EnableInterrupts();

// Halt the CPU until the next interrupt (or forever if they're disabled).
// Used by the idle loop and KernelPanic.
[[noreturn]] void HaltForever();

// ---- Memory barriers ----

// Full memory barrier (x86: mfence / ARM64: dsb sy).
void MemoryBarrier();

// Store barrier (x86: sfence / ARM64: dsb st).
void StoreBarrier();

// Load barrier (x86: lfence / ARM64: dsb ld).
void LoadBarrier();

// ---- TLB ----

// Invalidate the TLB entry for a single virtual address.
// x86: invlpg / ARM64: tlbi vale1is
void TlbInvalidatePage(uint64_t virtAddr);

// Flush the entire TLB (x86: reload CR3 / ARM64: tlbi vmalle1is).
void TlbFlushAll();

// ---- Page table root ----

// Read the root page table physical address.
// x86: mov cr3 / ARM64: mrs x0, ttbr0_el1 (or ttbr1_el1 for kernel space)
uint64_t ReadPageTableRoot();

// Write the root page table physical address.
// x86: mov cr3 / ARM64: msr ttbr1_el1, x0; isb
void WritePageTableRoot(uint64_t physAddr);

// ---- CPU info ----

// Spin hint (x86: pause / ARM64: yield).
void SpinHint();

// Returns the current exception/privilege level.
// x86: always returns 0 (ring 0) until user mode added.
// ARM64: mrs x0, CurrentEL >> 2
uint32_t CurrentPrivilegeLevel();

} // namespace brook::arch
