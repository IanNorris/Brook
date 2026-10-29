# Experimental ext2 write-behind

This opt-in experiment queues copied ext2 device writes so ordinary file,
namespace and inode updates can return before device completion. It is disabled
by default. It does not add an on-disk format or a journal.

## Configuration and memory

```
-fw_cfg name=opt/ext2-writeback-mib,string=4
-fw_cfg name=opt/ext2-writeback-ms,string=1000
```

The capacity is 0–16 MiB **per mounted filesystem**, default 0 (disabled).
The age threshold is 250–10,000 ms, default 1,000 ms. Missing or invalid values
use defaults. Options load after fw_cfg initialization and before block mounts.
The existing allocation-metadata threshold remains independently configurable,
with its 256-operation default unchanged.

A mount preallocates its byte arena and 4,096 record descriptors before
publishing itself. Allocation failure leaves that mount on the original
synchronous path. Writes never allocate queue storage or grow this fixed budget;
when capacity is exhausted they drain synchronously. This bounds additional
resident memory but does not make the preallocated arena reclaimable under
pressure. If its worker cannot be created, buffering falls back to synchronous
writes, draining previously accepted bytes first.

## Visibility, ordering and durability

All queue changes and drain I/O use the existing exclusive ext2 lock. Concurrent
readers use its shared side; they read underlying bytes and overlay pending
records in acceptance order. A containing pending record can supply the read
without touching the device. Existing inode and block-cache updates/invalidation
still happen when a write is accepted, so cache hits observe the same logical
state as queued reads.

Only the immediately preceding record can merge with an overlapping or adjacent
new write. Its contiguous union preserves old bytes outside the new range and
copies new bytes last. No intervening record boundary or flush boundary is
removed. This eliminates intermediate tail versions; it does not preserve every
intermediate on-disk value. Other records are submitted in their original order.

A drain writes records in order and stops at the first short/error completion.
Only after every write succeeds does it issue `DeviceFlush`. Records retire
only after the flush succeeds. Either kind of failure retains the complete
batch for ordered replay; newer writes cannot merge/admit past a failed batch
until it completes. Incomplete data/pointer/inode writes retain the existing
permanent mount `writeError`; flush-only errors remain retryable and failed
admission is also reported through the deferred-sync error.

Oversized writes first drain older records, then use the original synchronous
path. An oversized write's failed/short completion cannot be replayed from
owned storage, so it poisons subsequent queued writes and remains a permanent
mount error. Successful oversized bypass writes retain the old durability
contract: completion before return, explicit flush for stable storage.

Ext2 sync queues its dirty allocation metadata, drains all pending writes and
flushes the device before clearing bitmap/BGDT dirty flags. Vnode `fsync`,
`fdatasync`, `O_SYNC`/`O_DSYNC`, general sync and unmount retain their existing
paths to that full barrier. A failed unmount retains the mount and queue.

A kernel worker checks every 250 ms and drains queued batches whose oldest
copied write reaches the configured age. It also retries pending empty-batch
flush failures. Selection, drain and mount removal share the filesystem lock;
no unreferenced mount pointer crosses an unlocked interval. Shutdown stops the
worker and waits out any active drain before closing write admission and doing
its final filesystem/device sync.

The age is a scheduling threshold for copied queue data, **not a hard durability
deadline**: device stalls, filesystem-lock contention and oversized bypasses
are outside that promise. Ext2 remains nonjournaled; interrupted drains can
leave partial metadata updates. This does not claim crash consistency.

## Diagnostics and gates

`/proc/ext2writeback` reports per-mount capacity, queued bytes/records, input
write attempts, tail merges, submitted backend writes/bytes, total drains,
capacity-triggered drains, sync/background drains and failures. Submitted byte
counts include attempted replay, not only successfully completed bytes.
Use deltas with `/proc/ext2probe` and QMP counters around matched workloads.
When this queue is enabled, ext2 sync-body timings include draining ordinary
queued data as well as allocation metadata; they are no longer metadata-only
flush costs.

Hosted tests exercise the production queue with 3,000 randomized write/read
byte-oracle iterations, overlapping/prepended tail unions, descriptor and byte
capacity drains, oversized bypasses, partial writes, failed flushes, ordered
replay and rejection of newer writes across failed batches. The actual ext2
backend test enables buffering on a separate disposable image and checks
read-your-writes, fsync, failed-unmount retention, sticky short-OUT errors and
replay. Its stopped fixtures are checked independently with e2fsck.

`scripts/workloads/write_behind_check.py` is a disposable native namespace/data
oracle: creates and patches 32 files, shrinks files, renames, makes hard links
and a relative symlink, reads before sync, checks the O_SYNC sync hook, and
requires background progress when buffering is active. It refuses an existing
root. Its 39 deterministic regular-file hashes can be compared with the same
program on Linux and independent extraction from the stopped image. The
separate 8 MiB partial-write fixture covers additional random offsets, sector
boundaries, large writes and fsync/reopen behavior.

Interactive tail latency, deliberate host stalls, abrupt power loss during a
drain, and broader duplicate-mount alias behavior remain outside the initial
acceptance. Keep the feature opt-in until those risks are assessed.

## Initial native results, 29 October 2026

Implementation: `37bfa93d` on `perf/steam-loading-20261029`. Measured ELF:
`0a1cd88293582fef37750d20dc8a30cd140614949f6f5b4f74130d994ce37097`,
module ABI 57, with matching module inventories and passing entry/start audits.
The affected queue, actual-ext2, virtio-write-plan and file-object hosted suites
pass. Astra's read-only review found no data-integrity blocker; its empty-batch
retry observation was addressed before native tests.

The initial fresh-source pair uses 8 vCPU, 8 GiB RAM, MLFQ, Intel Venus,
headless/silent output, the same plain indexed Nix store, default OOM recovery,
and the unchanged 256-operation allocation threshold. Buffering is off in the
control and 4 MiB per mount/1,000 ms in the treatment. Actual capacities were
verified in the guest. Host caches/network state are uncontrolled. Native
warmup ran before Steam; the first off run omitted the treatment's 2.25-second
age-worker wait. Thus retain it as an initial result, not a tightly controlled
repeatability claim.

- Cache off: first qualifying Steam main window in **69.221 seconds**;
  data-disk QMP deltas 59,980 read requests and 104,883 write requests.
- Cache on: window in **62.397 seconds**; 35,379 read requests and 88,460 write
  requests. Approximately 16% fewer write requests and 41% fewer read requests
  at this endpoint are consistent with tail coalescing and reading pending
  bytes. These are guest requests, not physical SSD operations. Flush requests
  increased from 169 to 183; this result is not explained by suppressing flushes.

Queue counters sampled shortly after the buffered window show roughly 15,000
data-disk tail merges. Approximately 0.28 MiB remained queued across the mounts
at that snapshot. QMP and proc snapshots are separate observations while the
client can still write; do not treat their deltas as an atomic accounting.
The endpoint is window creation, not completed page rendering or interactivity.

Both first-pair runs closed Steam normally, passed checked shutdown with the
original launch owner exiting 0, and passed all six stopped-image data/Nix/tmp
filesystem checks. Independent extraction matches all 39 namespace-file hashes
and the 8,388,731-byte partial-write oracle at both settings. The latter's SHA256
is `a56e27fabef4d31336274695aaa4a3e7695bb8bd57b339159fd41914529466b0`.

**Confirmation did not establish a repeatable speedup.** A reverse-order pair
with the native warmup wait matched took 189.404 seconds with the cache off,
then left Steam helpers running after normal client Exit. The cache-on repeat
did not produce a qualifying window within 300 seconds. A one-second diagnostic
profile was taken late in that stalled repeat, so it is not an unprofiled
control. It has 746 CPU samples, 530 idle, 13,105 total events and no reported
loss/count mismatch. GPU completion waits dominate its non-idle samples;
queue processing is not identified as a CPU hotspot. This sparse system-wide
sample does not establish the cause of the application stall or exclude blocked
filesystem time. Do not compare 189 seconds against 62 seconds as a cache gain.

The failed repeats remain preserved and are excluded from normal application
exit/performance acceptance. Both completed the native namespace oracle. Their
existing operations were followed without relaunching Steam. Orderly OS
shutdown eventually completed with original-owner exit 0 and all six stopped
filesystem checks passing; no force termination was required. The off repeat's
39 files were independently extracted after shutdown. The on repeat also
completed the partial-write oracle, then its CPUs were paused and **all 39
namespace files plus the partial-write file were independently extracted and
hashed before any shutdown sync**. That confirms acknowledged sync had placed
the bytes on the backing image, rather than shutdown alone making them visible.
No deliberate host-power-loss guarantee is inferred from this check.

No observed device completion timeout, periodic-writeback failure or OOM victim
explains these repeats. Startup/CEF/helper-exit variability remains unresolved.
The initial I/O reduction is promising, but the timing evidence is insufficient
to enable buffering by default. Keep the implementation experimental and
diagnose that variability before conducting further Steam-based tuning sweeps.

Private receipts and images remain outside Git under
`build/reviews/steam-perf-20261029/writebehind-{on,off}-raw/` and
`writebehind-repeat-{on,off}-raw/`. Timing failures, process snapshots, diagnostic
profile and private Steam logs are retained alongside the successful integrity
checks. No game assets, credentials or account screenshots accompany this code.
