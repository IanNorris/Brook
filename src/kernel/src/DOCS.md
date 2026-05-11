# Brook Kernel — Architecture Documentation

## Overview

Brook is a 64-bit x86-64 SMP hobby kernel supporting preemptive multitasking,
virtual memory, loadable driver modules, a compositing window manager, TCP/IP
networking, and ext2/FAT filesystems. The kernel boots via a custom UEFI
bootloader, initialises hardware through ACPI/APIC, and runs user-mode ELF
binaries with Linux-compatible syscalls.

## Boot Flow

```
UEFI Bootloader
  → KernelMain (kernel.cpp)
    → Stack switch to BSP kernel stack
    → KernelMainBody
      → GDT/IDT/TSS setup
      → Physical/Virtual memory manager init
      → Heap init
      → ACPI parse → LAPIC/IOAPIC init
      → KVM clock calibration
      → Serial output init
      → PCI enumeration → device drivers
      → Filesystem mount (FAT boot, ext2 nix/data)
      → Scheduler init → SMP AP bringup
      → Compositor/WM init → framebuffer
      → Shell/init process launch
```

## Source File Map

### Core

| File | Purpose |
|------|---------|
| `kernel.cpp` | Entry point, BSP boot sequence |
| `gdt.cpp` | GDT/TSS setup (per-CPU kernel stacks, syscall segments) |
| `idt.cpp` | IDT, exception/interrupt handlers, signal delivery |
| `cpu.cpp` | CPUID feature detection |
| `smp.cpp` | AP bringup, per-CPU state, trampoline |
| `apic.cpp` | LAPIC timer, IOAPIC routing, IPI delivery |
| `acpi.cpp` | ACPI table parsing (MADT, FADT) |

### Process & Scheduling

| File | Purpose |
|------|---------|
| `process.cpp` | Process creation, fork, exec, exit, wait, fd table |
| `scheduler.cpp` | Per-CPU run queues, context switch, sleep/wake, idle |
| `sched_policy.cpp` | MLFQ policy: priority decay, boost, queue management |
| `elf_loader.cpp` | ELF loading, PT_LOAD/PT_INTERP/PT_TLS mapping |

### Memory

See `memory/DOCS.md` for PMM, VMM, heap, and page cache details.

### Synchronisation

See `sync/DOCS.md` for SpinLock, KMutex, KSemaphore, KRwLock.

### Filesystems

| File | Purpose |
|------|---------|
| `vfs.cpp` | VFS layer: mount table, path resolution, symlinks |
| `fatfs_vfs.cpp` | FAT12/16/32 via FatFs library (boot disk) |
| `fatfs_glue.cpp` | FatFs ↔ block device glue |
| `ext2_vfs.cpp` | ext2 read/write with block groups, indirect blocks |
| `procfs.cpp` / `procfs_vfs.cpp` | /proc filesystem (cpuinfo, meminfo, pid/maps) |

### Networking

| File | Purpose |
|------|---------|
| `net.cpp` | Ethernet RX/TX, ARP, IPv4, UDP, ICMP, DNS, socket API |
| `tcp.cpp` | TCP state machine, retransmit, OOO reassembly, congestion |

### Display & Compositing

| File | Purpose |
|------|---------|
| `display.cpp` | Framebuffer setup from UEFI GOP |
| `compositor.cpp` | Double-buffered compositor, dirty-region tracking |
| `window.cpp` | Window manager: chrome, focus, drag, resize, z-order |
| `clock_overlay.cpp` | Taskbar clock |
| `debug_overlay.cpp` | On-screen debug stats |
| `boot_logo.cpp` | Early splash screen |

### Terminal & Shell

| File | Purpose |
|------|---------|
| `terminal.cpp` | VT100/xterm terminal emulator with CSI parsing |
| `tty.cpp` | TTY layer: line discipline, job control, SIGWINCH |
| `shell.cpp` | Init process, environment setup, boot scripts |

### Input

| File | Purpose |
|------|---------|
| `input.cpp` | Input event registry, dispatch to WM or grabber |
| `keyboard.cpp` | PS/2 keyboard ISR, scancode → keycode |
| `mouse.cpp` | PS/2 mouse ISR, 3-byte packets → position/buttons |

### Audio

| File | Purpose |
|------|---------|
| `audio.cpp` | Multi-stream mixer, /dev/dsp interface, 8-stream mixing |

### Storage & Devices

| File | Purpose |
|------|---------|
| `device.cpp` | Block/char device registry |
| `pci.cpp` | PCI bus enumeration, BAR mapping, MSI-X |
| `virtio_blk.cpp` | VirtIO block driver with read-through cache |
| `fw_cfg.cpp` | QEMU fw_cfg device for early config |
| `ramdisk.cpp` | In-memory block device |
| `rtc.cpp` | CMOS RTC for boot-time anchor |
| `kvmclock.cpp` | KVM paravirt nanosecond clock |

### Diagnostics

| File | Purpose |
|------|---------|
| `profiler.cpp` | Sampling profiler: LAPIC-driven, RBP chain walk |
| `klog.cpp` | Kernel ring buffer log |
| `panic.cpp` | Kernel panic handler |
| `panic_screen.cpp` | Panic screen renderer with register dump |
| `panic_qr.cpp` | QR code encoding of crash state for host decode |
| `watchdog.cpp` | Per-CPU watchdog timer for hang detection |
| `serial_writer.cpp` | Buffered serial output |
| `ksymtab.cpp` / `ksym_addrs.cpp` | Symbol table for backtraces |
| `exception_info.cpp` | Exception name/description lookup |

### Loadable Modules

| File | Purpose |
|------|---------|
| `module.cpp` | ELF module loader with ksymtab relocation |

See `../drivers/DOCS.md` for loadable driver modules (intel_hda, virtio_net,
virtio_input, xHCI, etc.).

## Key Design Decisions

### Compositor: Double-Buffer with Dirty Tracking

The compositor maintains a cached-RAM backbuffer and only copies dirty scanlines
to the MMIO framebuffer. MMIO writes are 10–100× slower than cached RAM writes,
so this avoids full-frame MMIO flushes (~8 MB at 1920×1080×4).

### Syscall Interface: Linux-Compatible

Brook implements ~200 Linux syscalls via LSTAR/STAR MSRs. User programs compiled
against musl or glibc can run without modification. The syscall table lives in
`syscall.cpp` (~12,000 lines) and covers: process management, file I/O, memory
mapping, networking (socket/connect/bind/listen/accept/send/recv), signals,
futex, epoll, inotify stubs, and Brook-specific WM syscalls (512+).

### Scheduling: MLFQ with Per-CPU Queues

The scheduler uses a multi-level feedback queue with priority decay and periodic
boost. Each CPU has its own run queue. Context switches save/restore integer
registers, FPU state (FXSAVE/FXRSTOR), and per-CPU running flags. The timeslice
is 10 ms.

### Signal Delivery

Exception handlers (vectors 13/14) push all GPRs into a `FullExceptionFrame`,
allowing signal handlers to modify the return context. `SA_RESTART` support
rewinds RIP over the `syscall` instruction for interrupted reads.

### VirtIO Block: Cached Polling

VirtIO block uses synchronous polling with a 16 MiB per-device read-through
cache (4 KiB direct-mapped). Large reads use 64 KiB coalesced I/O. Writes
invalidate overlapping cache entries. The driver uses a ticket lock (not
KMutex) to avoid scheduler interaction in the I/O path.

### Process Address Space Layout

```
0x0000_0040_0000          ELF load base (USER_LOAD_BASE)
  ... heap (brk, up to 128 MiB) ...
0x0000_1000_0000          mmap region start (USER_MMAP_BASE)
  ... mmap allocations grow upward ...
0x0007_0000_0000_0000     mmap region end (USER_MMAP_END)
  ... guard ...
0x0000_7FFF_FFFE_0000     stack top (USER_STACK_TOP, 8 MiB default)
```

### Profiler Output Format

The sampling profiler writes a text-based event stream to `/boot/PROFILE.TXT`:
```
PROF_BEGIN <cpuCount> <startTick>
P  <tick> <pid> <cpu> <flags> <rip>     ; Sample (flags bit 0 = user/kernel)
CS <tick> <cpu> <oldPid> <newPid>        ; Context switch
PROF_END <totalSamples> <dropped>
```

A host-side Python script converts this to Speedscope JSON for flame graphs.

### Networking: Single-Threaded RX

All network packet processing (Ethernet → ARP/IPv4 → TCP/UDP → socket queue)
runs single-threaded to avoid contention. TCP uses a pure state machine in
`tcp.cpp`. Socket waiters are woken via futex/poll mechanisms.

### Loadable Modules

Optional drivers (audio, graphics, network, input, USB) are compiled as
separate ELF binaries and loaded at boot via `module.cpp`. Symbol resolution
uses the kernel symbol table (`ksymtab`). Modules register IRQ handlers and
device ops, and must unregister on unload.
