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
