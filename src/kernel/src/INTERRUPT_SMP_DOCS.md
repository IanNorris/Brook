# Interrupt, CPU & SMP Subsystem Documentation

## Overview

Brook's interrupt and SMP subsystem handles x86-64 exception/IRQ dispatch,
LAPIC/IOAPIC configuration, multi-processor startup, and per-CPU state.

## Files

| File | Lines | Purpose |
|------|-------|---------|
| idt.cpp | 1825 | IDT setup, exception/IRQ handlers, signal delivery |
| idt.h | 41 | IDT interface |
| gdt.cpp | 190 | GDT/TSS setup (BSP + per-AP) |
| gdt.h | 84 | GDT interface, segment selectors |
| apic.cpp | 756 | LAPIC timer, IOAPIC, IPI, IRQ routing |
| apic.h | 110 | APIC interface |
| cpu.cpp | 171 | CPU feature detection (CPUID) |
| cpu.h | 111 | KernelCpuEnv, MSR constants |
| smp.cpp | 575 | AP bootstrap, SMP init, panic NMI halt |
| smp.h | 66 | SMP interface, CpuHaltedState |
| exception_info.cpp | 90 | Human-readable exception descriptions |
| exception_info.h | 14 | Exception info interface |

## Architecture

```
Hardware interrupts / exceptions
  → IDT dispatch (idt.cpp) — vector 0-255
    → Exceptions 0-31: fault handlers with diagnostics
    → IRQs 32-47: IOAPIC-routed device interrupts
    → Vector 0xFD: LAPIC timer (profiler + scheduler tick)
    → Vector 0xFE: Reschedule IPI (wake idle CPUs)
    → Vector 0xFF: Spurious
  → SWAPGS for user→kernel transition
  → Per-CPU KernelCpuEnv via GS segment
```

## Key Mechanisms

### Exception Handling
- Vectors 0-31 handled by `HandleExceptionFull()`
- IST1 used for #DF (double fault) — separate stack
- User-mode faults: signal delivery or process kill
- Kernel-mode faults: panic with register dump + QR code

### LAPIC Timer
- Calibrated against PIT during early boot
- Adaptive rate adjustment (`ApicAdjustTimerRate`)
- Drives profiler sampling and scheduler ticks
- Preemption only from user-mode (kernel non-preemptible)

### SMP Startup Sequence
1. BSP parses MADT for AP LAPIC IDs
2. Trampoline code copied to physical 0x8000
3. Each AP: INIT IPI → SIPI → trampoline → long mode → spin-wait
4. BSP calls `SmpPrepareAPs()` — allocates per-CPU GDT/TSS/env
5. BSP calls `SmpActivateAPs()` — signals APs to enter scheduler

### Reschedule IPI (Vector 0xFE)
- Sent by `KickIdleCpu()` when a process becomes runnable
- Naked ISR handler: SWAPGS + full GPR save/restore
- Foundation for future TLB shootdown

## Audit Findings (2026-05-10)

### Overall Assessment
**Fundamentally sound.** The interrupt and SMP subsystems are well-designed
with correct SWAPGS handling, proper IST stack usage, and robust AP
synchronization. All identified issues are in initialization code that
runs single-threaded before SMP is online.

### Issues Found (all LOW-MEDIUM severity)
- `GdtInitAp`: `g_cpuTssCount` modified without lock (safe: single-threaded)
- `IdtInstallHandler`: writes IDT without lock (safe: init-only)
- `IoApicUnmaskIrq`: read-modify-write without lock (safe: init-only)
- `SmpCurrentCpuIndex`: early-boot LAPIC ID lookup is racy (mitigated by GS-based fast path)
- Panic NMI handler lacks SWAPGS (safe: only accesses RIP-relative data)

### Recommendations
- Add spinlocks to init functions for future CPU hotplug support
- Document that `SmpPrepareAPs` must precede `SchedulerInit`
- Consider per-CPU panic counter for nested panic detection
