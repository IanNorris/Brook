# Loading throughput controls — 26 October 2026

Stardew gameplay is user-verified and marked compatible on main, with slow
loading explicitly retained as a limitation. The earlier hard-link/inode-index
change reduced one request-to-window observation from 333.5 to 128.3 seconds;
subsequent same-kernel observations varied substantially. This pass adds controls
and repairs the test harness, not a new production performance optimization.

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
