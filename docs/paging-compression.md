# Paging and compression — staged implementation

Branch: `wip/paging-compression-20261028`. Goal: reduce memory-pressure failures
and improve mixed-workload throughput without trading them for corrupt mappings
or unbounded reclaim stalls. This document records a plan, not shipped paging.

## Delivery order

1. Plain 4 KiB pages on a dedicated, explicitly formatted disposable backing
   device. First validate the slot store and I/O failures independently, then
   explicit pageout/refault of opted-in private anonymous memory. Establish
   exclusion against new aliases/pins, mapping revalidation, full TLB retirement,
   unmap/exit cancellation and byte integrity before automatic reclaim.
2. Bounded compressed-RAM pool behind the same ownership interface. Keep codec
   workspace, metadata and fault-in reserves resident; bypass incompressible
   pages and spill cold entries to the disk tier. Measure latency and CPU as
   well as saved bytes. A full pool must not cause a retry/compression loop.
3. Independently indexed compressed immutable Nix-store blocks, with a versioned
   format, checksums and host list/inspect/extract/verify/export tools. User data
   initially remains standard ext2. Writable compression is a later consistency
   project. Resident file pages remain decompressed unless explicitly reclaimed.

The first kernel milestone must not infer eviction safety from a PMM refcount
snapshot. CLONE_VM peers, fork/CoW aliases and retained kernel/DMA mappings need
an explicit exclusion rule. MappingPinRegistry pins vnode lifetimes, not anonymous
frames. Existing demand-zero faults must consult paged-out state first.
MappingTransaction retains the AS and cancellation region, but its destructor
ACKs then releases detached references: retain a separate source pin through
pageout I/O or add an acknowledged-retirement phase. No stale completion may
resurrect a replaced mapping. Ordinary user-copy retries may resolve faults;
arbitrary kernel faults under unknown locks may not perform disk reads.

Current virtio-blk I/O is synchronous and polls under its request guard. A pager
worker can keep the requester asleep but does not itself eliminate backend
polling. No AS/PT/pager spinlock may cover disk I/O. The I/O path needs its own
bounded forward-progress assessment before pressure-driven reclaim is enabled.

## Codec investigation

Do not equate a large streaming benchmark with a 4 KiB paging workload. Compare
independent 4 KiB pages and 64/256 KiB file blocks, reporting ratio, compression
and decode MB/s, decode latency, workspace and memcpy control. Include executable
bytes, structured/text data, zero/sparse pages and incompressible controls.
Use round-trip validation and a volatile checksum to prevent dead-code removal.
Host measurements are screening evidence; freestanding guest code has no SIMD
register permission today and must be measured separately before integration.

Initial candidates are LZ4 1.10.0, LZ4HC for offline-built file blocks, Zstd
1.5.7 at fast/low levels and LZAV 5.18. Newer is not automatically faster. LZAV
is a scalar alternative with a ratio/speed tradeoff, while LZSSE depends on SIMD;
it cannot be dropped into Brook's general-register-only kernel paths without
an explicit register-state contract. Keep on-disk codec IDs versioned.

Primary references:
- https://github.com/lz4/lz4
- https://github.com/facebook/zstd
- https://github.com/avaneev/lzav
- https://github.com/ConorStokes/LZSSE
- https://www.kernel.org/doc/html/latest/admin-guide/mm/zswap.html

## Acceptance

Track available/resident/paged/compressed bytes, metadata and pool use, pageout/
fault-in counts and latency, failed/skipped reclaim, disk bytes and client frame
p95 under a fixed memory cap. Demonstrate more useful working set or fewer
failed launches and acceptable tail latency; paging is not a universal speedup.
Reject corrupt/short I/O, stale slot tokens and slot exhaustion deterministically.
Run mapping/fork/unmap/exit stress and clean shutdown/fsck on private test disks.
