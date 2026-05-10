# Loadable Driver Modules Documentation

## Overview

Brook's loadable driver modules are kernel ELF objects loaded at runtime
via the module system. They export init/exit functions and import kernel
symbols through the ksymtab relocation mechanism.

## Drivers

| Driver | Files | Purpose |
|--------|-------|---------|
| **intel_hda** | intel_hda_mod.cpp (~900 lines) | Intel HD Audio controller: PCM playback via BDL/DMA ring |
| **virtio_input** | virtio_input_mod.cpp (~750 lines) | VirtIO input device: keyboard/mouse/tablet events |
| **virtio_net** | virtio_net_mod.cpp (~720 lines) | VirtIO network: TX/RX virtqueues, interrupt-driven |
| **virtio_rng** | virtio_rng_mod.cpp (~280 lines) | VirtIO RNG: entropy collection via virtqueue |
| **bochs_display** | bochs_display_mod.cpp (~220 lines) | Bochs/QEMU VGA: mode set via VBE dispi registers |
| **ps2_kbd** | ps2_kbd_mod.cpp (~180 lines) | PS/2 keyboard driver (port 0x60/0x64) |
| **ps2_mouse** | ps2_mouse_mod.cpp (~200 lines) | PS/2 mouse driver (IRQ12) |
| **sched_mlfq** | sched_mlfq_mod.cpp (~350 lines) | Multi-Level Feedback Queue scheduler policy |

## Architecture

```
Module loader (module.cpp)
  → ELF parse + relocation against ksymtab
  → ModuleInit() call → driver registers with subsystem
  → ModuleExit() call → cleanup on unload

Driver init typically:
  → PCI device scan/claim
  → MMIO BAR mapping
  → Virtqueue/DMA buffer allocation
  → IRQ handler registration via IoApicRegisterHandler
  → Subsystem registration (InputRegister, NetRegister, etc.)
```

## Audit Findings (2026-05-10)

### Known Issues
- **BRO-128**: virtio_input and virtio_net don't unregister IRQ handlers on
  module unload — dangling handler pointer causes crash on interrupt
- intel_hda: No memory barrier between BDL entry writes and DMA register
  setup — hardware may read stale BDL entries
- intel_hda: PlayLock doesn't cover HdaIsPlaying()/HdaGetPosition() reads
- virtio_net: Spinlock released before NetReceive in RX loop — window for
  duplicate packet processing
- virtio_rng: Entropy pool accessed without synchronization from concurrent
  callers
- bochs_display: PCI BAR size not validated before MMIO access

### Overall Assessment
**Functional for QEMU targets.** The virtio drivers follow the spec
correctly for single-queue operation. Main risks are the missing IRQ
teardown on unload (crash if modules are reloaded) and the HDA DMA
barrier (could cause audio corruption on some hardware). The modular
architecture is clean and extensible.
