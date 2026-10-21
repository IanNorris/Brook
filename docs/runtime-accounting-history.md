# Bounded runtime accounting history

`sched runtime history` is an opt-in diagnostic for service-floor corrections.
It records the wall/steal pairs already consumed by the runtime ledger, their
owner identities, floors and resulting charges. It does not add clock/MMIO
reads, change the sampling retry decisions, reset a ledger or waive errors.
Recording is off by default. Enabled copying and the disabled condition checks
have nonzero observer cost; new native captures do not replace uninstrumented
acceptance results.

## Commands

- `sched runtime history status`: metadata for CPU0, without copying entry rows.
- `sched runtime history on`: start an empty capture, or preserve an already
  enabled capture. Repeated `on` does not erase a positive event.
- `sched runtime history off`: stop recording while preserving the capture.
- `sched runtime history reset`: clear history metadata and advance its epoch,
  preserving enabled/disabled state. Runtime counters and service floors remain
  unchanged. Explicit reset discards previous history evidence.
- `sched runtime history cpu N`: export one online CPU's ordered rows.

Stop recording before exporting multiple CPUs. Each reply is copied under the
existing ready lock and formatted after releasing it; multiple replies are not
one atomic snapshot while recording continues. Check matching epochs and final
sequence numbers across the stopped exports. The command uses the normal debug
thread and is not panic-safe.

## Capture bounds and interpretation

A fixed ring holds **64 total events per CPU**. Before a clamp it rolls. The
first recorded clamp globally triggers sixteen additional events on each CPU,
then that CPU freezes. Ownership events count toward the sixteen. A full
triggering-CPU ring therefore contains at most 47 preceding events, the clamp
and sixteen later events, rather than 64 preceding samples plus a tail.
Some other CPUs may have little preceding history or may not complete their
tails before recording is stopped.

`triggered`, `trigger_cpu`, `trigger_sequence` and `trigger_sample` identify the
first captured clamp. `sequence` orders recorded events across CPUs under the
existing serialization; the ledger's `sample` number also includes events
outside the retained history. Raw pairs come from the existing bounded
steal/wall/steal reader. `attempts` records successful reader attempts; zero is
used by synthetic/failed samples and ownership events. Shared-record versions
are not captured by this diagnostic.

Each sample row contains its owner before/after, raw input, floor before/after,
validity/baseline flags, outcome, and thread-service or idle credit. An `owner`
row records a ledger ownership change using the last accepted raw pair; its
`valid:false` and `attempts:0` distinguish it from a fresh sample. PID and
incarnation must both match when following a task. Owner identity alone does
not establish whether actual execution was in user mode, kernel or IRQ code.

`trigger_caught_up` means the triggering CPU subsequently supplied a valid pair
whose wall-minus-steal reached the triggering floor. It does not certify correct
task attribution of an earlier delayed correction. `catchup_sequence` identifies
that retained row. `baseline_lost` means invalid/backwards input interrupted
the triggering CPU's post-trigger interval; it is not a statement that earlier
history was complete. A lost baseline prevents a catch-up claim even if a later
new baseline has a larger absolute value.

Check bounds explicitly:

- `count` is retained rows; `entries_included:false` deliberately omits them in
  metadata-only replies.
- `prefix_overwritten` counts evicted prefix entries, including those displaced
  by the bounded tail. It does not indicate an unlimited history.
- `frozen` is per CPU; `all_frozen` means every captured CPU completed its tail.
  Stopping early can leave both false.
- `omitted_after_freeze` counts later events on an already-frozen CPU only while
  other CPUs still record. It stops increasing once all CPUs freeze.
- `overflow` indicates record-sequence exhaustion. Prefix/omission counters
  saturate rather than wrap; their exact values are not an unbounded guarantee.
- Reaching the tail bound without catch-up is incomplete correction evidence.
  Enabling during an existing baseline or rolling away older events also limits
  what can be inferred about the origin of a floor.

The records retain both successful and failed decisions. A positive capture
remains failed runtime acceptance; policy/ledger agreement or a clean shutdown
does not make its correction benign. See [the retained failures](runtime-clamp-evidence.md).

## Verification

The actual ledger tests compare recording enabled/disabled across 40,000
sample/owner events, PID generation changes, invalid/backwards inputs and
history resets. Every returned charge, task/group total and diagnostic field
must agree. Focused cases reproduce the native 4,887-ns correction, change
owner before catch-up, check exact tail/prefix composition, preserve duplicate
`on`, prevent false catch-up after an unknown interval, and retain unresolved
debt when the tail expires. Metadata-only reads leave entry storage untouched.
All 70 current host tests pass with the wrapped Clang 18/libc++ toolchain.

The native diagnostic capsule changes only the kernel and symbol-address table;
all thirteen modules remain byte-identical to the normal-placement comparison.
Module ABI 34 is unchanged and all module imports resolve. Native positive-clamp
capture and causal interpretation are recorded separately; deterministic tests
alone cannot establish how host publications caused a particular correction.

The first native diagnostic run used eight vCPUs, the original 12-KiB HDA
module and optional EEVDF, with history explicitly enabled after verifying its
default-off state. Four fresh Firefox launches, eight finite guest CPU workers
during the first launch, and finite four/eight-fragment OSS drains completed.
The capture recorded 15,971,750 events across 11,870,090 ledger samples without
reproducing a clamp. After stopping recording, all eight exports matched epoch
and sequence and retained 512 ordered rows with consistent floor and owner
transitions. Policy/ledger totals agreed. The original VM owner exited zero and
all four offline filesystem checks passed.

This verifies native collection and export, **not a timing repair or EEVDF
acceptance**. The earlier positive failures remain open, and enabled history
can affect the workload. Evidence and the verification receipt remain ignored
under `build/reviews/unattended-runtime-history-eevdf-a-20261021/runtime-history-control`.
A focused clock/accounting workload is the next capture attempt; this negative
browser run does not justify changing sampling decisions or clearing errors.
