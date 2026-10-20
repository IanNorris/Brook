# HDA silence reservation and copy diagnostics

The [small-queue controls](audio-small-queue-pacing.md) show repeated short silence
without a pitch change. Immediate producer delivery helps that reproduction but
fails to establish a consistent mixed desktop benefit. Before changing HDA's
conservative lead, this probe measures the intervals the driver actually reserves.
It changes no scheduling policy, queue capacity, publication check or completion
accounting. The existing module ABI remains 34; two new named kernel exports
support the diagnostic HDA module without changing `AudioDriver` or receipt layout.
New modules require those exports; use the matching frozen kernel/module capsule.

## Controls and interpretation

The normal debug command pipe accepts `audio delivery on`, `off`, `reset` and
`status`. It starts disabled with epoch zero. Reset clears interval counters and
recent events, preserves enabled state and advances the observation token. Enable
and disable also advance it, so an old in-flight copy cannot enter a newer epoch.
A lifetime contention counter is deliberately not reset.

Each accepted native HDA copy reports physical pending bytes before reservation,
reserved silence bytes, accepted bytes, copy clock brackets, the existing
post-copy device advance, and physical headroom. Startup copies are counted but
excluded from running-position/headroom statistics. Watermarks include unowned
silence and never represent application completion. An unsafe publication remains
rejected and its exposed receipt remains subject to the existing discontinuity
rules. The probe does not turn a failed sample into proven playback.

Enabled diagnostics add one pre-copy stable KVM clock read. Existing post-copy
clock and position samples supply the other endpoints; there are no extra MMIO
reads. Copy duration excludes reservation zeroing and device setup;
the copy-and-position bracket includes the existing position read. Device advance
starts at the driver's preceding position observation, so it spans reservation
work as well as copying. It is not a device-rate estimate from copy time alone.
Headroom is the receipt's lead over that preceding completion watermark minus
observed advance, clipped to zero if DMA caught the write.

Aggregate counters and a seven-bin copy-duration histogram cover the enabled
interval. Edges are 1 us, 10 us, 100 us, 1 ms, 10 ms and 100 ms; bins are lower
inclusive and upper exclusive, with the final bin unbounded. Sixteen recent events
are returned oldest first; they are not a full timeline or a worst-case sample
set. Separate `max_advance_event` and `min_headroom_event` objects retain the
actual samples responsible for those extrema, with their own clocks and lead.
Position validity also requires the existing publication sample clock order
and age checks, while a valid caught-write sample retains zero headroom.
Invalid samples, counter saturation or dropped records must be inspected
before inferring bounds. Absence of a large advance in a short run cannot justify
relaxing the publication guard.

The HDA playback lock serializes samples. Recording only tries the independent
diagnostic lock and drops contended records rather than waiting for a reader.
Snapshot/control operations never poll or lock the driver. The disabled path
checks an atomic token through the kernel hook and adds no timing/MMIO samples.
This is diagnostic instrumentation, not proof that disabled overhead is zero.
Software-queue-to-device residence time remains a separate measurement to add.

## Verification

The pure model tests disabled/stale tokens, reset, exact reservation totals,
histogram bins, invalid clock/position observations, caught-write headroom,
startup exclusion, recent-event wrap and saturating counters. The hosted test
using the production HDA implementation checks an exact reservation and device
advance during copy, then intentionally advances DMA past the receipt and verifies
that the publication guard still rejects it. Disabling recording leaves counters
unchanged. All 68 hosted suites pass. The diagnostic kernel and all 13 modules
build, with all 108 imported kernel symbols resolved. Supported entry-state and
mutation audits pass; the existing unsupported entries remain outside that audit.

## First native measurements

Fresh private disks, eight vCPUs, six GiB, MLFQ/default BSP wake scanning,
callback HDA, SDL into a private 44.1-kHz null sink and a 20-ms requested host
buffer reproduce the existing 30-s numbered-tone direct writer. There are no CPU
stress workers. Each run proves exactly 1,323,000 completed frames, 5,292,000
accepted physical application bytes and successful synchronization.

| Negotiated stream capacity | Completion | Silence reservations | Reserved bytes / 176,400 B/s | Remaining excess over source + reservations |
| --- | ---: | ---: | ---: | ---: |
| 4,096 frames | 34.023 s | 454 / 702,208 bytes | 3.98077 s | 42.36 ms |
| 8,192 frames | 30.034 s | 1 / 1,504 bytes | 8.53 ms | 25.88 ms |

Reserved silence accounts for almost all the smaller queue's extra duration.
The remaining interval includes startup/drain/observation effects; these counters
do not prove exact physical-speaker timing. This supports a driver reservation
interaction, rather than a change of pitch or an explanation of the historical
accelerated playback. Queue geometry is read back from the guest before timing.

Across these runs, maximum measured copy time is 25.687 / 49.795 us; maximum
copy-plus-position bracket is 2.172 / 2.529 ms. The greatest observed device
advance is 3,584 bytes in each run. Minimum remaining headroom is 10,752 / 12,288
bytes. Extrema may come from different writes: they must not be combined into an
imaginary single sample or treated as worst-case bounds under host contention.
A later refinement retains the exact maximum-advance and minimum-headroom events.
Both the 8,192-byte idle trailer and 12,288-byte publication lead stay unchanged.

All eight native control/fault cases pass. No invalid clock/position sample,
unsafe publication, diagnostic lock drop or counter saturation is recorded.
The probe starts disabled at epoch zero; histogram totals equal observation
counts and accepted-byte totals match the writer. Original launcher/router owners
exit normally; four filesystem checks, saved-data preservation, exported result
size/hash, heap integrity, runtime accounting and frozen identity checks pass.
The frozen initial capsule uses kernel
`d5d7be6433ddd93d9c403a31de125e34ac60ec1d9f9ae9e37b6a56a8fa8b7f96`
and HDA module
`81de6967b1c17f2fba1b7f8635174048760a81695a75ebf90a29d7ff4e943748`.
The other twelve modules match the accepted prefix-refill control exactly.

Ignored evidence is under `delivery-probe-*`. The first probe's raw position
validity bit checked range/overflow; the refinement additionally qualifies sample
clock order/age. All native observations above passed the full existing
publication predicate, so that distinction does not invalidate these samples.
Focused tests verify that stale samples cannot enter refined headroom extrema,
and that valid caught-write faults still retain zero headroom. The refined
capsule is `delivery-extrema-frozen`, kernel
`f94ba4c6bdafdd85c46bb60360b08d58d5f3b7757bbe5dd4680d462700cc68a8`
and HDA `2290b9900c2f2b19e3cd72cfe29d7ad2ca258024477f59fc918b1334e34a3dad`.

The refined capsule also completes the same native Quake GL3 generated-track
run. No invalid sample, unsafe publication, diagnostic contention or saturation
is recorded; the default-off status, exact extrema JSON and all existing
lifecycle/filesystem/accounting gates pass. There are 899 accepted submissions,
3,690,496 application bytes, and 340 reservations totaling 779,776 physical
bytes over the complete application run, including load/startup/shutdown phases.
That aggregate must not be divided by the demo interval as though it covered
only that interval.

Snapshots surrounding the roughly eight-second PCM capture differ by 120
reservations / 262,656 bytes, equivalent to 1.489 s of physical silence. These
are surrounding snapshots, not sample-exact capture boundaries. Reservations
can include future queued intervals and an already traversed unowned gap if
DMA has passed the old submission end. They do not map one-for-one onto quiet
samples in the captured window. The generated-track decoder reports source
progression 0.8306, pitch approximately 1.0000, and 15.67% quiet 1-ms bins,
with consecutive decoded source IDs.

The retained maximum-advance and minimum-headroom event is the same write:
13,056 bytes pending, no new reservation, 4,096 bytes accepted, 3,584 bytes of
observed advance and 9,472 bytes remaining headroom. Its copy bracket is
1.033 us and its pre-copy-to-post-position bracket is 95.398 us. Device advance
starts at an earlier position observation, whose timestamp is not exposed by
this version: **95.398 us is not the full advance interval** and must not be
used to derive an instantaneous device fetch rate. The maximum copy-and-position
bracket over the whole run is 7.479 ms, from a different event. The copy maximum
is 4.272 us. This is further evidence to measure MMIO/host delay separately
from copying and to preserve the exact publication predicate.

Astra's read-only review agrees with the reservation interpretation and recommends
matched off/on observer controls and representative contention before any lead experiment. Diagnostic
reads/recording may perturb timing; no reduction in safety margin follows from
these unloaded measurements alone. This instrumentation is not a playback fix.
