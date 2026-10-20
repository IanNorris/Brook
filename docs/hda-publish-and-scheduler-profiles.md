# HDA publication evidence and short scheduler profiles

The intermittent HDA `publish-unsafe` fault from the
[normal-request controls](eevdf-normal-request-control.md) is not fixed or
reproduced again here. Its old record omitted the exact post-copy samples.
The driver now preserves those samples and the failed safety checks, without
changing its publication decision, acceptance limits, restart policy or ABI.

## Publication diagnostics

The first fault per epoch still stops DMA, clears its buffer and logs the proven
completion watermark. For a publication rejection it now also records:

- `before_ns` / `after_ns`: the existing post-copy and post-position clock reads.
- `raw_position`: the exact existing device-position read.
- `raw_completed`, receipt `begin`, `accepted` byte count and position `delta`.
- `sampled`: whether running DMA required the position sample.
- `checks`: a bit mask of failed predicates, on a second line with the same epoch.

| Bit | Check |
| --- | --- |
| 1 | Post-copy clock unavailable |
| 2 | Invalid normalized position |
| 4 | Post-position clock unavailable |
| 8 | Clock ordering invalid |
| 16 | Sample interval reaches the half-ring ambiguity limit |
| 32 | Raw completion plus delta overflows |
| 64 | Sampled raw completion plus delta exceeds the new receipt's beginning |

Several bits can coexist. Interpret completion/delta only when position and
clock validity are established; an invalid position does not prove DMA overtook
the write. Existing samples are reused: there are no additional hot-path clock
or MMIO reads. Diagnostics are constructed only after the unchanged predicate
rejects publication; serial logging remains after DMA stops.

Production-driver hosted tests independently inject DMA overtake, a stale
sample, backwards/unavailable clocks and an invalid device position. They verify
retained timestamps/receipt/position fields, first-fault retention, refusal of
further writes, the unchanged completion watermark, and a stopped zeroed ring.
`test_audio_delivery` passes. The native build resolves all imports for thirteen
modules against 106 kernel exports. No device safety check was weakened.

## Native diagnostic collection

Four fresh private eight-vCPU/six-GiB boots use the generated browser/video/audio
fixture plus 24 CPU workers for 30 seconds. The kernel and twelve modules remain
byte-identical to the 4-ms baseline; only the HDA diagnostic module changes.
EEVDF uses its unchanged 4-ms normal request, with MLFQ as a comparison. Audio
uses callback HDA, native PipeWire/private null output and requested 20-ms
buffering. These are profiled diagnostic cohorts, not unprofiled performance
controls or speaker acceptance.

| Run | Policy | Capture | CPU samples | Stored switches | Producer drops |
| --- | --- | --- | --- | --- | --- |
| hp-e1 | EEVDF | 1,000 ms | 765 | 23,974 | 1,168 |
| hp-m1 | MLFQ | 1,000 ms | 764 | 20,639 | 0 |
| hs-e1 | EEVDF | 250 ms | 193 | 5,261 | 0 |
| hs-m1 | MLFQ | 250 ms | 193 | 2,000 | 0 |

The one-second EEVDF export is lossy and is retained separately. Shortening the
capture removes that producer loss in both policies; all stored event totals
agree with the reported totals. No exact proportions are inferred from the
lossy export. Even a loss-free capture does not uniformly sample IRQ-masked
work, and a context-switch count is not CPU service or wait duration.

The corrected controller's 1.1-second grace after READY is present in these
runs. All four audio windows are proven wholly inside the conservative CPU-load
bounds. All buffered page clock models pass with no missing page trace samples.
No native HDA fault, QEMU codec overrun or timer-adjustment event occurs in these
four trials. That does not establish that the intermittent fault is gone, nor
that audio is continuous or correctly paced in every real workload.

## What the profiles establish

Exact frozen kernel symbols and the actual module load/section layout resolve
the kernel stacks. The loss-free short EEVDF capture contains 1,000 outgoing
context-switch stacks through `UserMappingPageAccess`, often under
`UserBufferReadable`, `UserBufferWritable` or the user-copy path. Among its top
80 per-task records, known Firefox tasks account for 887 such stacks. The
loss-free short MLFQ capture contains 675. The one-second loss-free MLFQ capture
contains 11,541. These counts describe their different observed intervals;
they do not prove a stable rate or a policy speed ratio.

Source inspection identifies an exclusive `AddressSpaceState::mutation` mutex
in `UserMappingPageAccess`, including read-only PTE/range checks. User-copy
preflight checks each page through that function. This establishes a concrete
contention path shared by many Firefox threads, rather than proving that their
longest input delay was spent there. CPU-job threads created after the first
process snapshot may be unnamed after they exit; they are not silently renamed
as Firefox tasks or assumed to belong to a group from nearby PID numbers.

GPU completion polling also appears in the CPU samples: the short EEVDF capture
has fourteen kernel leaf samples in `SubmitCommand`'s wait/pause lambda; short
MLFQ has eighteen there and four in `GpuWaitForCompletion`. These are observed
samples, not percentages of all elapsed time. Block-device polling appears too.
An earlier one-ms GPU-sleep experiment made presentation worse; these samples
do not justify reintroducing it or attributing every stall to the scheduler.

Next measure wait and hold durations for the address-space mutation lock, its
owners and operation classes, and GPU completion latency separately from mutex
queue time. Use those to choose between reducing unnecessary exclusive
validation/lock handoffs and improving completion notification. Any memory-path
optimization must preserve range permissions, CoW, cancellation protection,
address-space lifetime, concurrent unmap/protect and fault-retry semantics.
Do not replace those checks merely to obtain a faster synthetic result.

The subsequent [preflight timing probe](mapping-preflight-timing.md) measures
wait/check brackets under both policies. It preserves the distinction between
initial validation, other lock holders and scheduling after handoff.

## Collection gates and retained evidence

All four runs pass clock/runtime anomaly checks on all eight CPUs, heap
integrity, tracked app/CPU exit-zero with no live descendants, empty final audio
streams, original-wrapper/router exit-zero, four stopped filesystem checks and
the twelve saved documents/four original profile-tree preservation checks.
EEVDF service totals equal the runtime ledger and exposed queue counts agree.
MLFQ lacks those debug contracts; absent debug counters are not an invariant
pass. All VMs and both private services stopped normally.

Ignored evidence under `build/workloads/timing-scheduler-20261020`:

- `hda-publish-profiles`, `hda-publish-short-profiles`: frozen controllers,
  launchers, source hashes, actual QEMU arguments, profile quality, raw exports,
  exact-symbol analyses, buffered traces, PCM and lifecycle receipts.
- `hda-publish-profile-results.json`: collection validation and exact common
  kernel/module identity, with the lossy export explicitly marked.
- `hda-publish-frozen`: common-artifact manifest, diagnostic driver, exact source,
  production-driver test, analyzer and collection validator.
- `hda-publish-evidence-build.log`: native build/import checks.

Private review names are `timing-mix-{hp-e1,hp-m1,hs-e1,hs-m1}-20261020` under
`build/reviews`. Their serial logs and failure/preservation evidence remain
available. These results do not change either scheduler or audio defaults.
