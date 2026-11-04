# Dell Broadwell forcewake experiment

On the XPS 13 9343 (`8086:1616`, revision 9), the explicitly enabled
`dell-gpu-wake-20261104` payload booted from the inactive SSD slot and returned
online with the writable desktop ready. This one-shot experiment reserved
Brook's own forcewake request bit, requested wake, observed the blitter twice,
then released the bit. It assumes firmware/SMM does not concurrently use that
same bit; Brook cannot prove that from an ACK read.

The boot-pinned `bridge hardware gpu-gop-plan` result reported exact inventory,
D0, all four admitted pages, valid aliases and an advancing calibrated clock.
It recorded acquired and released ACK, thread C0, and two equal idle samples:
head 0, tail 0, mode 0x200. Clear delivery and released ACK both succeeded.
The hardware image and update receipt are private build artifacts under
`build/hardware/dell-gpu-wake-20261104` and
`build/hardware/dell-gpu-wake-deploy-20261104`; guest boot ID was
`6c2b35b3b80749aaadb6f3403be625d8`.

This does not establish exclusive engine or GGTT ownership, DMA identity,
rendering capability, or permission to modify the firmware framebuffer. The
experiment requires the separate `/etc/dell-gpu-forcewake-run` marker and the
existing TSC calibration marker. It verifies effective UC mappings and request
page write permission under the kernel page-table lock, uses fixed DWORD MMIO
and a device posting read, and never retries in the same boot. Any uncertain
release excludes further use by this owner for the boot. It can hold the lock
with interrupts disabled for the bounded polls, and is therefore a diagnostic
boot experiment, not a production rendering path.

The kernel built, six targeted host tests passed, including admission, denied
fourth page, clock failure, D3, posting failure, cleanup, and mapping
permissions. A private QEMU/OVMF fixture reached the desktop with the same
kernel/initrd pair and did not enter the Dell-specific access path.

## Ring and execution-list observation

A subsequent clean boot of `dell-bcs-context-20261104` (guest boot
`828215fae71c471fb411c875bad2e17f`) repeated the guarded forcewake operation
and also sampled BCS ring start, ring control, and the low and high
execution-list status DWORDs. Both samples matched: ring start `0`, control
`0`, head and tail `0`, mode `0x200`, status low `0x1`, status high `0`.
The wake request was released and the SSD desktop returned online.

These are individual register reads, not an atomic 64-bit snapshot. A stable
ring-idle sample does not establish an empty execution queue; status bits need
separate interpretation. In particular, no inference about ownership of the
engine, display allocations, other engines or GGTT slots follows from this
observation. The register addresses follow the [Linux i915 engine register
definitions](https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/gt/intel_engine_regs.h.html#178).
The matched image reached the desktop in a disposable QEMU/OVMF boot before
the Dell run; the four affected host tests passed.
