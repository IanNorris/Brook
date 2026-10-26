# Loading throughput controls — 26 October 2026

Stardew gameplay is user-verified and marked compatible on main, with slow
loading explicitly retained as a limitation. The earlier hard-link/inode-index
change reduced one request-to-window observation from 333.5 to 128.3 seconds;
subsequent same-kernel observations varied substantially. This pass repairs the
test harness, adds cache/mount-fanout controls, and implements conservative
mount-lookup hints. Application measurements and their limits follow below.

## Reproducible small I/O gate

The synthetic harness had silently depended on privileged bridge launches.
Ordinary launches now default to the Brook user, so creating fixtures directly
under root-owned `/data` failed with `EACCES`. The probes now explicitly request
UID 0 and record that identity. This is a test-fixture fix, not evidence of a
new filesystem regression or of unprivileged app compatibility.

All runs use the same ABI-55 kernel SHA256
`3fb62053bca2733caf4a136ac91f3bd8de965147a68863f6a86682570cb3c33f`,
8 vCPUs, 2 GiB guest RAM, eight workers, 8 MiB per worker and 64 small files per
worker. RAM and disk gates pass mixed-append, boundary/fragmentation, content
verification after reboot, cleanup and offline checks, returning free blocks
and inodes exactly to their starting values.

The new `--backend disk --host-cache cold-cleanup` control waits for the first
VM's owner to reap QEMU, finishes offline checks, fsyncs only the private image
inodes, requests `POSIX_FADV_DONTNEED`, and measures their page residency before
and after. It rejects incomplete eviction. It does not evict host filesystem
metadata or device caches and never changes global cache settings.

Two consecutive runs with durable controller ownership both exit zero:

- Normal cache: whole guest gate 32.58 s; sequential write 50.6 MiB/s,
  sequential read 85.9 MiB/s; reboot verification of 64 MiB takes 338.95 ms.
- Cold cleanup: whole gate 34.13 s; sequential write 60.7 MiB/s,
  sequential read 68.1 MiB/s; reboot verification takes 333.00 ms. All three
  images have zero resident host file pages immediately before the second boot.

Only the reboot/cleanup stage follows eviction. Initial write/read phases cannot
be attributed to that cache control. Boot itself can repopulate pages before
verification. These results do not prove equivalence for large application loads.

Host samples cover the entire controller interval, including Nix setup and
offline checks, rather than exactly aligned guest phases. CPU `some` pressure
accounts for 15.2% and 16.8% of those intervals; host load ranges from 0.93–7.23
and 9.60–16.16 respectively. Global I/O `some` pressure is 0.52% and 0.40%.
These are shared-host measurements, not workload-exclusive counters. The runs
are useful correctness/cache controls, not a quiet-host A/B performance claim.

Earlier small cached runs reached 238–354 MiB/s on sequential reads. Another
cold-control attempt completed guest checks but its outer controller exited
143; it is retained as incomplete controller acceptance and excluded from the
successful pair. Its initial phases were already slower before eviction.

## What to measure next

The small gate does not reproduce the real load: Stardew startup previously
issued roughly 334–414 thousand data-image reads, with around 1.0–1.3 GiB read.
Aggregate QEMU request service times overlap; they are not an additive wall-time
breakdown. Neither these counters nor the cache controls justify blaming one
guest function for the full delay.

The next bounded experiment should separate Steam runtime preparation from game
initialization, then compare metadata/path lookup cost at low and high bind-mount
counts. Prior captures include retained mount/path walking and synchronous
virtio block I/O. A mount-fanout probe with unchanged file content would test
that lead cheaply before changing mount indexing. Pair this with actual request
size/count measurements on the existing application images; throughput of fresh
small images is insufficient evidence about their layout or cold behavior.

Raw reports, cache residency, original-controller exit receipts and host samples
remain under ignored `build/workloads/loading-throughput-20261026/`. No game,
account, save or authentication data is published.

## Mount fanout experiment and candidate

The added `--mount-fanout-probe` uses the same checked one-byte file throughout
three stages: before, during and after 128 unrelated bind mounts. Each stage
contains three batches of 500 absolute lookups and 500 directory-FD-relative
lookups, including content/EOF verification on every iteration. It separately
checks overmount restoration and retained-FD identity. Linux passes the same
fixture in a private user/mount namespace, without the roughly doubled cost
observed in Brook's short samples.

The complete Brook baseline passes with original controller exit zero, clean
offline filesystem checks and exact allocation restoration. Median time per
checked absolute / directory-FD lookup is 26.49 / 29.84 microseconds before
fanout, 53.18 / 57.98 during fanout and 24.71 / 30.17 after removal. These
include open, byte/EOF reads and close, not just the pathname walk.

The candidate adds two conservative per-attachment hints: whether an attachment
may contain a covered mount, and whether it may parent a virtual mount ancestor.
False permits skipping the corresponding global registry scan; true retains
the existing exact lookup. Hints only transition to true. They never cache a
mount result or make a detached entry authoritative. Virtual chains mark every
actual parent attachment; namespace clone, recursive-bind publication and
pivot-root preparation also record new edges. Detach/failure may retain a true
hint, costing a scan without changing behavior.

Candidate kernel SHA256:
`e30a5e2940a6191319e0eba7c74665e1debb99d5e00dbfbb3713211eb089d2ae`.
The same native gate passes with controller exit zero and exact allocation
restoration. Its medians are 24.33 / 29.02 microseconds before fanout, 42.48 /
45.55 during fanout, and 20.54 / 24.97 after removal. The high-fanout reductions
are about 20% and 21%; this is not a measured game-loading improvement.

The existing production-VFS host fixture passes namespace copies, recursive
binds, virtual ancestors, pivot-root, file-bind aliases, failure rollback and
retirement/lifetime checks. Both compiled entry/scheduler mutation audits pass.
All 109 host tests pass across the validation: 108 in the full suite and the
unchanged FD stress test separately in 5.52 seconds. The latter was interrupted
in the first full-suite attempt after saturating the shared host while a review
VM was open. That attempt is not a passing full-suite result, and the contended
VM interval is excluded from performance comparisons. Future stress runs should
be separated from application timing.

Two earlier fixture attempts are excluded from acceptance: one used unsupported
`unshare` instead of Brook's supported mount-namespace `clone` path; another's
legacy launch reply timed out even though the probe completed. The latter was
recovered with checked shutdown and clean offline checks. The corrected gate
waits longer for that one launch reply and never retries a launch after timeout.
An incomplete candidate capsule failed before any VM launch and was replaced by
one containing its matching symbol table. Successful controls are named
`io-mount-fanout-owned-baseline` and `io-mount-fanout-candidate-complete` in the
private evidence directory.

## Real Steam/Stardew acceptance and first matched loading pair

The candidate opens the signed-in Steam client and renders its Library and
Store. One warm-client observation reaches Stardew's process after 41.28 s and
window after 53.04 s without a profiler during that launch. That observation is
not a matched comparison with earlier 128–212 s runs. It reads 1,049.8 MiB from
the data image in 336,019 requests, with 29.94 s of overlapping aggregate read
service time. The game renders its menu and has an active clocked audio stream.
After normal game close, a one-second QEMU mixed-output capture contains only
zero samples; no stale last-buffer playback is detected in that capture.

The subsequent matched pair clones both runs from the same checked, stopped
private disks, using 8 vCPUs, 8 GiB RAM, the same Intel graphics/display/audio
configuration and ordinary Steam launch path. All six private image inodes have
zero measured file-page residency before each launcher starts. The launcher can
repopulate pages; filesystem metadata and device caches remain uncontrolled.
There is one run per kernel, in baseline-then-candidate order.

- Baseline: Steam main window observed after 72.32 s; game process after
  157.20 s and game window after 169.38 s from its launch request. Data-image
  reads are 1,306.87 MiB in 406,137 requests; writes 161.74 MiB; aggregate read
  service 53.75 s.
- Candidate: Steam main window observed after 61.10 s; game window after
  75.78 s from its request. Data-image reads are 1,285.89 MiB in 400,036
  requests; writes 158.42 MiB; aggregate read service 42.98 s.

This pair observes about 55% less request-to-window time, with similar data
traffic. It is encouraging application evidence, not a repeated statistical
benchmark or a promise of the same reduction on other loads. Shared-host CPU
pressure, order effects and uncontrolled lower-level cache state remain limits;
service times overlap and cannot form an additive wall-time breakdown.
Whole-controller CPU `some` pressure is materially different: 16.43% for the
baseline and 0.35% for the candidate. These intervals include startup and
shutdown, not just the game load, and do not attribute pressure to a particular
process. Therefore the pair does **not** establish a causal 55% code speedup.
A reverse-order control is required before strengthening that claim.

That reverse-order pair is now complete, using fresh copies of the same seed
and the same pre-launch file-page control. The candidate reaches the game
process at 66.52 s and window at 77.78 s; the baseline reaches them at 85.31 s
and 98.12 s. Steam main-window observations are 61.99 and 60.76 s respectively.
Whole-controller CPU `some` pressure is 0.34% in both runs, versus the substantial
imbalance in the first pair. The candidate's data reads are 1,287.29 MiB in
400,416 requests; baseline reads 1,307.42 MiB in 405,973. Their overlapping
aggregate read-service times are 40.30 and 42.87 s.

The quieter reverse pair observes about **21% less game request-to-window
time**. Candidate window times are consistent across the two pairs (75.78 and
77.78 s); the first baseline's 169.38 s remains a contended observation rather
than the basis for a 55% optimization claim. Much of the quiet-pair difference
precedes creation of the game process, consistent with the runtime-preparation
target. These remain two observations per kernel on a shared host, not a broad
application benchmark. No further performance change is pursued in this pass.
Whole-QEMU process CPU observations in the reverse pair are 532.90 CPU-seconds
for the candidate and 544.99 for baseline, spanning most of the VM interval
rather than an exclusive game-load phase.

Both games close through their normal WM_DELETE protocol. Both Steam groups
drain with exit zero, audio streams drain, checked filesystem/device sync
completes, original VM owners exit zero, and all four offline filesystem checks
pass. All 33 original save content hashes and non-marker mtimes remain unchanged.
Cloud-marker mtime changes remain separate from gameplay save writes. The
candidate's earlier standalone review also passes these checks. Private pair
evidence is under `build/reviews/stardew-loading-ab-20261026/`.
Both reverse-pair runs pass the same acceptance checks; their evidence is under
`build/reviews/stardew-loading-ba-20261026/`.

## Additional-game installer investigation remains open

Graveyard Keeper's owned-game install dialog appears, but confirmation does not
start a download or create an app manifest in these runs. Input/focus controls
after the first confirmation do not establish whether the initial callback
blocked or whether the modal received input. Neither Graveyard Keeper nor
Untitled Goose Game is accepted or marked compatible by this pass.

A transient CEF reason-213 failure preceded recovery of the Library; a normal
client restart subsequently renders the Store. In Chromium 126.0.6478.183,
reason 213 is `INVALID_INITIATOR_ORIGIN`, a semantic origin authorization check.
The visible `data:` URL alone does not explain it. No kernel IPC repair or
cache reset is justified by that observation.
[Exact-version check](https://github.com/chromium/chromium/blob/126.0.6478.183/content/browser/renderer_host/ipc_utils.cc).

The next bounded discriminator is to test checkbox/hover/Cancel on a fresh
modal **before** pressing Install, then reopen, press Install once and repeat
the same harmless control. Responsive-before/dead-after supports a blocked
installation callback; dead-before keeps initialization/routing ahead of it.
If necessary, inspect recursive X11 child/focus/map/transient relationships
rather than assuming the large root child is the actual modal input target.
