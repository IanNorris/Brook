# Panic, Debug & Profiler Subsystem Documentation

## Overview

Brook's panic/debug subsystem provides crash diagnostics (serial + screen +
QR code), kernel logging, and sampling-based profiling.

## Files

| File | Lines | Purpose |
|------|-------|---------|
| panic.cpp | ~350 | KernelPanic entry, register dump, crash info |
| panic_screen.cpp | ~230 | Graphical panic screen with register/stack display |
| panic_qr.cpp | ~300 | QR code generation for crash data (QR v3-v10) |
| profiler.cpp | ~550 | Sampling profiler: LAPIC timer sampling + file writer |
| klog.cpp | ~50 | Kernel message log ring buffer |
| kprintf.cpp | ~200 | Kernel printf implementation |

## Architecture

```
KernelPanic()
  → CLI (disable interrupts)
  → SmpHaltAllAPs() (NMI broadcast)
  → Serial dump: registers, stack, backtrace
  → PanicScreenRender() — graphical crash screen
  → PanicQrGenerate() — QR code with crash URL
  → HLT loop

Profiler:
  LAPIC timer ISR → ProfilerSample() (push to ring buffer)
  ProfilerThread → ProfileWriterDrain() → VfsWrite to PROFILE.TXT
```

## Key Mechanisms

### Panic Output
- Serial output first (always available, lockless during panic)
- Graphical screen: register state, exception info, stack hexdump
- QR code: encodes crash data as URL for external decoder
- Double-panic detection: global counter prevents infinite recursion

### Profiler
- SPSC ring buffer between ISR producer and writer thread consumer
- Samples: RIP + PID + timestamp at ~1kHz from LAPIC timer
- Context switch events recorded with FROM/TO process info
- Writer thread drains to `/boot/PROFILE.TXT` (FatFS)

## Audit Findings (2026-05-10)

### Critical Issues
- **BRO-119**: `FillRect()` in panic_screen.cpp has no framebuffer bounds
  checking — can write past allocation if coordinates exceed fb dimensions
- **NMI panic race**: `KernelPanic` from NMI context calls `SmpHaltAllAPs()`
  which sends NMI IPI — could deadlock if target CPU is already in NMI

### High Priority
- Profiler SPSC ring buffer has a theoretical ABA race window between
  `readPos` check and data read (mitigated by ACQUIRE/RELEASE ordering)
- RBP frame walker in crash dump doesn't validate against stack guard pages

### Medium Priority
- Profiler writer uses synchronous VFS I/O which can block on disk locks
  (mitigated by recent flush-frequency fix)
- QR code packet buffer size not statically asserted
- Panic screen font atlas pointer not validated before use

### Low Priority
- Double-panic counter is global rather than per-CPU
- Exception descriptions are static strings (acceptable for diagnostics)
