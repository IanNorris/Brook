# Steam loading: bounded performance pass, 29 October 2026

The three changes on `perf/steam-loading-20261029` reduce time to Steam's first
visible main window by **20–24%** in three paired private-image comparisons.
Stardew's cold timings overlap baseline; they do not establish a repeatable
cold-load improvement. Strict warm comparisons improve by approximately 3–7%.
These endpoints measure window creation, **not full menu readiness**.

## Changes

- `7e5ecfff`: disjoint mapping insertion uses the existing AVL index to find
  neighbours instead of repeatedly scanning the sorted mapping list. Fork's
  ordered mapping construction was quadratic. Overlap rejection, backing
  references, identity allocation and list/tree consistency remain intact.
- `658905cf`: anonymous discard validates and locks the affected address-space
  interval without locking unrelated file mappings; CoW uses the existing
  fault-range lock. Generation revalidation and leaf preflight remain required.
- `bc644299`: synchronous partial-sector writes reuse resident cache sectors
  instead of always prereading disk. A miss rereads the entire required span;
  successful writes patch existing cache entries. Failed OUT and failed SG
  attempts invalidate the complete attempted span before retry/fallback.
  Writes remain synchronous; this introduces no delayed-write durability policy.

## Matched controls

Each run uses fresh private reflinks of the same checked, signed-in source:
8 vCPU, 8 GiB RAM, MLFQ, Intel `/dev/dri/renderD128`, Venus, headless output,
silent host audio, IRQ11 level 1, 4,096-frame audio staging, unbounded tmpfs,
and default OOM recovery enabled. No accepted run killed an OOM victim.
The signed-in source images were preserved.

“Plain” means the uncompressed indexed `/nix/store` image, compared with the
same indexed namespace compressed with LZ4. `/data` remains ext2 in both.
The readonly store is mounted from the pinned virtio5 device; serial mount
records and QMP backing identities were checked. Steam/runtime traffic is
predominantly on `/data`, so this is not a claim that compressing the store
alone accelerates Steam.

Final timings in seconds, rounded to one decimal:

| Pair | Steam baseline → final | Stardew cold baseline → final | Stardew warm baseline → final |
| --- | --- | --- | --- |
| Plain, final first | 85.0 → 65.4 | 100.4 → 79.9 | 55.0 → 73.4* |
| LZ4, baseline first | 88.0 → 66.9 | 93.1 → 80.6 | 78.4 → 73.1 |
| Plain, baseline first | 83.7 → 66.7 | 74.5 → 81.0 | 77.1 → 74.6 |

*The first pair's warm barrier waited for the window to disappear. Later pairs
also required all Stardew-named tasks to retire before the five-second warm
wait. Keep the first warm result as exploratory evidence, rather than combining
different definitions or hiding the slower result. The stricter warm results
show modest improvement; cold-load variance remains substantial.

No profiling or host image preparation/build ran during these final timed
controls. Host caches were not globally flushed, and network/account state was
not controlled. Thus “cold” means first game launch in a fresh guest, not cold
physical SSD cache. Reversing the plain pair's order retained the Steam gain.

Private delayed captures of the final plain run show a complete, recognizable
Stardew title menu without obvious corruption by 35 seconds after the warm
window endpoint, and again at 65 seconds. Earlier five-second captures were
mostly white in both baseline and final runs. There is no matched baseline
menu-readiness measurement, so the delayed observation is visual acceptance,
not an additional timing improvement.

## What the measurements explain

In the first plain pair, QMP's data-disk read-request count falls from
136,282 to 58,273 during Steam launch, while writes remain similar
(106,404 to 105,206). Cold game read requests fall from 146,012 to 58,318,
with writes 122,473 versus 123,124. This is approximately **57–60% fewer guest
disk read requests**, not an equivalent reduction in physical SSD reads or
bytes. Cache reuse avoids partial-write read/modify/write prereads.

A 20,000-range insertion microbenchmark improves from 2,783,390,156 ns to
1,888,490 ns (about 2.78 seconds to 1.89 milliseconds). This isolates the
quadratic mapping path; it does not predict whole-application speedup.
The individual exploratory application passes were variable, so only the
matched combined controls support the headline result.

Sparse valid CPU profiles identify repeated fork mapping insertion,
anonymous-discard file-gate scanning and synchronous block waits. Captures
with dropped/count-mismatched events and duplicate profile epochs are excluded.
Neither sample counts nor cumulative block-wait/QMP service counters provide
an additive wall-time partition: CPUs, requests and phases can overlap.

## Correctness and evidence

All six final controls closed the game and Steam normally, completed checked
shutdown with the original launch owner exiting 0, and passed independent
offline checks of data, Nix and tmp filesystems: **18 filesystem checks**.
Their serial logs contain no observed virtio-blk completion timeout, panic or
periodic-writeback failure.

The native partial-write probe writes 8,388,731 bytes using 600 deterministic
random partial writes, then fsyncs, reads and reopens the file. Independent
offline extraction from every stopped data image matches the Linux-host oracle:

```
SHA256 a56e27fabef4d31336274695aaa4a3e7695bb8bd57b339159fd41914529466b0
FNV64  469802d10d54160d
```

This probe establishes byte correctness, not speedup. Its baseline/final plain
times (0.443/0.481 seconds) are too small/noisy for a throughput claim.

Affected hosted suites pass: `test_mapping_ranges`, `test_mapping_record_reserve`,
`test_vmm_leaf`, `test_file_object`, `test_virtio_read_plan` and
`test_virtio_write_plan`. Coverage includes actual CoW and multi-node anonymous
discard while an unrelated file gate is held, replacement during unlock,
2,000 randomized write byte-oracle cases, cache misses/collisions, failed
preread/OUT, successful multi-SG writes and partial SG failure before bounce
fallback. Kernel entry/start contract audits pass for the exact measured ELF.

The measured baseline ELF is
`c8a10f938aed9efc8914158d8a9af797f3bef53cd3bb8810fbbefe250c270957`;
the measured final ELF is
`ca63ca200192fde5eaa0244d0ec489c5cf11cc7df1e00b4c9368a226077152da`.
Both use module ABI 57; frozen modules/import inventories were verified.
Raw receipts, private frames and images remain under
`build/reviews/steam-perf-20261029/`, outside Git. No account data or game assets
are included in this report.

## Exclusions and next leads

An early profiled final-kernel attempt overlapped host image preparation and
reported a virtio-blk completion timeout/reset and periodic-sync EIO. The
client did not become usable and normal application shutdown did not finish.
Evidence and disks were preserved; checked sync before QMP termination and
offline filesystem checks subsequently passed. Host load is a possible
contributor, **not an established root cause**. This run is excluded from timing
and normal-quiescence acceptance; the completion-timeout recovery path remains
a stability follow-up.

Other exploratory controls include an observer timeout while the guest kept
running and an early shutdown receipt read before the supervisor published its
exit code. Their original operations were followed rather than relaunched;
incomplete timings and missing original-owner evidence are not promoted to
final acceptance results.

The profiler also exposed a pending-drain interval incorrectly reported as
idle. A separate diagnostic follow-up keeps status busy and rejects a new
capture until that handoff finishes. Application timings above precede this
diagnostic-only change. The worker's legacy path when a capture ends before
it first runs still deserves separate coverage.

The diagnostic follow-up ELF
`1060c660bea14630b119f0c47c657c3c0a67f776f22885ba5be3a725d42cc85c`
passes the same entry/start audits. Three consecutive native one-second captures
have distinct epochs, complete end markers, matching event counts and zero
reported drops (5,300; 3,028; 3,096 events). That private smoke run also completes
checked shutdown, original-owner exit 0 and three offline filesystem checks.

The next useful loading target is repeated SteamRT filesystem/runtime setup and
metadata write volume on `/data`. More aggressive write coalescing, dirty-cache
management and crash durability require a purposeful follow-up; they are beyond
this two-hour pass. There is no demonstrated reason here to replace LZ4 or
increase image-cache limits.

## Metadata batching follow-up

`opt/ext2-auto-sync-ops` configures the allocation/free operation threshold per
mount at boot. Its default remains 256; valid decimal values are 1–65,536.
Missing, zero, malformed or out-of-range input falls back to 256. For example:

```
-fw_cfg name=opt/ext2-auto-sync-ops,string=4096
```

This changes the operation-count batching of allocation bitmaps and group
descriptors. It does not create a general write-behind data cache, change the
five-second writable-mapping worker, or defer explicit `fsync`/mount sync.
Larger values extend the unsynced allocation-metadata window; this is not a
wall-clock interval, and ordinary writes, inode updates and directory writes
remain synchronous.

`/proc/ext2probe` reports one row per live physical-drive mount, with cumulative
automatic, vnode-fsync and generic mount-sync counts and nanoseconds spent
inside those sync bodies. Generic mount sync includes `sync()`, periodic
shared-mapping writeback and unmount; it is not exclusively automatic or
application initiated. Time waiting to acquire the filesystem lock and orphan
cleanup before the inner sync body is not included. Counters are mount-lifetime
snapshots under the filesystem lock; destroying and recreating a mount resets them.
Use deltas around the workload, alongside `/proc/blkprobe` and QMP counters.

The production-backend hosted test performs 4,096 allocation/free operations at both 256 and
4,096 thresholds, checking 16 versus one automatic device flush. It also
verifies a forced sync drains pending metadata before the larger threshold,
and that a failed automatic flush retains dirty metadata, retries successfully
and reports its deferred error. Invalid option parsing is covered. These
fixtures subsequently pass independent filesystem checking.

Two fresh private-image Steam controls use the same audited ELF
`f8bf2b38ef598aa995b50e1a7ee638804ba27bec567fa1698bbe3a0256535bcb`,
the same source and launch configuration as above, and verified effective
thresholds in the guest. The 4,096-operation treatment ran first, followed by
the 256-operation control; no host preparations/builds overlapped their timed
launches. This is one pair, not a repeatability study:

- **256:** first Steam window in 66.600 seconds; 70 automatic allocation-sync
  calls across the writable mounts, taking 0.690 seconds in their sync bodies;
  111 vnode-fsync calls taking 0.744 seconds; 42 generic mount-sync calls taking
  0.165 seconds. Total inner sync-body time: 1.599 seconds.
- **4,096:** window in 65.562 seconds; no automatic allocation-sync calls;
  explicit/general syncs drained the metadata before reaching that threshold.
  There were 105 vnode-fsync calls taking 0.715 seconds and 42 generic
  mount-sync calls taking 0.318 seconds. Total inner sync-body time: 1.033 seconds.

The data disk reports 104,993 versus 98,679 writes and 168,347,136 versus
153,121,792 written bytes. Application activity and explicit-fsync counts vary
too; do not attribute that entire difference or the roughly one-second launch
gap to the batching knob. In particular, these sync counters exclude ordinary
synchronous writes and any canonical-page writeback before the filesystem sync
hook. They do not partition the remaining application wall time.

Both runs closed Steam normally, completed checked shutdown with the original
launch owner exiting 0, and passed all six offline data/Nix/tmp filesystem
checks. The native partial-write probe and independent offline SHA256 extraction
match the same 8,388,731-byte Linux-host oracle above at both thresholds. No
inner sync errors, device completion timeouts or periodic-writeback failures
were observed. Entry/start contract audits and the production ext2 hosted suite
pass. Private receipts are `batch-final-256-raw/measurement/`,
`batch-4096-raw/measurement/` and `batching-comparison.json` beneath the existing
review directory.

**Decision:** retain the 256-operation default. Eliminating automatic allocation
flushes removes less than a second of measured direct sync work here; this
does not justify expanding the unsynced-metadata window by default. The option
and counters remain available for experiments. A bounded general write-behind
cache for ordinary file/inode/directory writes is the more substantial next
avenue, requiring explicit durability, ordering, eviction, pressure and
failed-completion handling rather than just a larger bitmap batch.
