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

## Submission-mode check

A follow-up boot (`c23a72f70fab42e28d59be1df44d3629`) sampled the BCS
submission-mode register twice as zero, i.e. legacy rather than execlist mode
at that instant. Both samples again reported head, tail, start and control zero,
mode `0x200`, and execlist status low `0x1`, high `0`. Broadwell documents
bit 0 as the current-list pointer, not a pending-work flag; the low status
value is the documented default. The decoded result is a quiescent-looking
BCS submission sample, not an exclusive handoff or GGTT lease. Other engines,
display allocations and future submissions remain to be accounted for.

The matched image passed a disposable QEMU boot and four focused host tests,
including changed context, valid-ring and nonempty-execlist cases; Astra
reviewed the register offsets and predicate. The Dell returned online with its
SSD desktop ready and forcewake released. The [Broadwell register
reference](https://www.x.org/docs/intel/BDW/intel-gfx-prm-osrc-bdw-vol02c-commandreference-registers_0.pdf)
and [Linux i915 definitions](https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/gt/intel_engine_regs.h.html#192)
identify the decoded fields.

## Four-engine candidate

The opt-in `/etc/dell-gpu-engines-run` path extends the guarded wake
observation to the Broadwell GT2 render, video and video-enhancement engines,
using the same forcewake acquisition as BCS. It requires all seven register
pages to pass resource and mapping checks before the first write. Each sampled
engine reports both raw register sets; a failed or all-ones read stops further
engine reads and proceeds to release. A successful set of sequential samples
is not an exclusive handoff and cannot establish that all engines were idle at
one instant or that display allocations are accounted for.

The four targeted host tests passed, including last-page rejection before any
MMIO write, pending video work, read failure and all-ones cleanup, opt-in gates,
and bridge serialization. The exact matched ABI-65 image booted to the desktop
in private QEMU; the Dell was offline, so no physical multi-engine read or
handoff is claimed. Astra reviewed the register whitelist and failure path.
The [Linux Broadwell forcewake selection](https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/intel_uncore.c.html#2228)
and [engine register definitions](https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/gt/intel_engine_regs.h.html)
provide the implementation references.

## Context and first-copy preparation

Brook can now construct a fresh Broadwell BCS 64-bit logical-context image,
a first-level PPGTT copy batch and a privileged-ring GGTT completion write.
The command words and descriptor were checked against Linux i915's Gen8
context layout and emission routines. The completion address checks the Gen8
alignment and bit-5 restriction; the descriptor is published only after all
other output is built, including when the caller aliases it with an input.

The GGTT binding transaction now accepts up to 16 pages, enough for a context,
multi-page ring, status page and initial buffers. Host tests cover all 16
write-failure positions with delayed writes, restoration, duplicate mappings,
context image fields, complete batch/ring words, bad ranges and output aliasing.
A composed test derives the context and ring addresses from one reserved GGTT
lease and uses a separate PML4 DMA address. Astra reviewed the encodings and
failure tests. These helpers remain unconnected to a physical submission path:
GPU/firmware ownership, display accounting, engine HWS/CSB handoff, page-table
publication, physical backing isolation and safe retirement are still required.
A completion write alone does not permit freeing the context or ring.

The one-shot execution interface now also requires the backend to establish
execlist mode, an unstopped ring and owned context-status readiness before
publishing or binding GPU-visible pages. The Dell's observed legacy mode must
fail this gate. Host tests simulate legacy, unreadable or unstable mode, a
stopped ring and unavailable status ownership; each rejects before publication,
GGTT, HWS or ELSP writes, after which the caller releases prepared pages. The test backend supplies
these answers; no physical readiness reader or mode transition is implemented.

`BdwExeclistObservationReady` provides the observation portion of that future
gate: it checks a successful stable, quiescent two-sample observation for
run-list enable and a clear STOP_RING bit, and requires a separate CSB ownership
assertion. The [Linux engine register definitions](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/gt/intel_engine_regs.h)
identify those bits. The helper does not establish the supplied ownership or
prevent a later submission, and no real execution backend consumes it yet.

References: [Linux Gen8 XCS context layout](https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/gt/intel_lrc.c.html#108),
[batch-start emission](https://github.com/torvalds/linux/blob/v6.8/drivers/gpu/drm/i915/gt/gen8_engine_cs.c#L509-L565),
and [completion write restrictions](https://github.com/torvalds/linux/blob/v6.8/drivers/gpu/drm/i915/gt/gen8_engine_cs.h#L108-L130).
