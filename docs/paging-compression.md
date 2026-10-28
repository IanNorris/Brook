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
1.5.7 at fast/low levels and LZAV 5.20. Newer is not automatically faster. LZAV
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

## First backing-store checkpoint

The new `memory/page_store` primitive uses caller-owned resident records and a
bounded raw page area. Slots have generation-tagged handles, immutable stored
bytes, explicit references, full-length I/O checks and an accidental-corruption
checksum. It never changes a PTE or claims that its caller may evict a frame.
All calls are externally serialized; ownership, I/O buffer stability and backend
lifetime remain caller obligations. The format is disposable and does not
persist its runtime slot index/checksums across a reboot.

`scripts/page_store_image.py create <new-file> --mib 4` creates a sparse raw
backing image; `inspect` validates its format/capacity/identity. Creation refuses
existing paths. An opt-in boot diagnostic is selected with
`-fw_cfg name=opt/page-store-probe,string=virtio1` and a separately attached
recognized image. It runs before tasks/module unloading, bounds its metadata to
4096 slots, and performs no writes on an invalid header. No device is attached
as an ongoing pager by that diagnostic.

The host store test covers actual file-backed roundtrips, capacity, stale tokens,
reference lifetime, short/error reads/writes and corruption. Image-tool tests
cover create/inspect, no overwrite and damaged/truncated headers. Kernel builds
pass. A fresh 8-vCPU hardware VM using the exact staged kernel reports:
`page-store probe: PASS device=virtio1 writes=4 reads=4 live=0 error=0`.
After clean checked shutdown and owner exit 0, independent host reads match all
four patterns exactly. Artifacts: `build/paging-compression-20261028/native/`.
**Application eviction, fault-in and memory-pressure improvement are not yet
implemented or claimed.**

## Codec screening result

Current stable upstream releases are LZ4 1.10.0 and Zstd 1.5.7. The newer scalar
LZAV candidate was pinned to revision
`b93c884dab90ac16112f78daed9ff6e07c3d2fac` (header version 5.20), SHA-256
`28b9cca2fc91c230575dd65e9ab7c7ed4ed3aad7c2aea543093e897c31d81b04`.
This source is an ignored benchmark reference, not a new kernel dependency.

One ordered host screening run over a frozen copy of the built ELF, in
independent 4 KiB blocks, found LZ4 ratio 1.9896, encode
767.1 MB/s and decode 3618.6 MB/s
(~1.132 microseconds/block). LZAV was ratio
1.9937 and decode 2794.2 MB/s;
Zstd fast3 ratio 2.0155/decode 1393.4 MB/s;
Zstd level1 ratio 2.5568/decode 777.4 MB/s.
LZ4HC-9 achieved ratio 2.1410/decode 3969.6 MB/s,
so it is a plausible offline file-image option rather than the first paging
compressor. The memcpy control was 44617.6 MB/s: none of
these compressed-data decoders is literally memcpy-speed on this input. An
initial screening had somewhat higher absolute rates with the same ranking;
these single ordered samples are not statistically controlled performance gates.
Source/text, zero/sparse and random controls, and 64/256 KiB blocks are included
in the local CSV. These are synthetic/file corpora, not captured anonymous
browser pages. Frozen input SHA-256s and exact library paths are recorded in
`build/paging-compression-20261028/codec-metadata.json` and `codec-command.txt`.

Reproduce with `scripts/workloads/benchmark_page_codecs.cpp`, compiled at -O3
against the exact external LZAV header and loaded LZ4/Zstd library paths. Each
codec roundtrips before timing; contexts/buffers are reused where exposed, and
a volatile result byte prevents deletion of copies. LZAV's default compressor
may allocate; it is not yet a verified reclaim-safe entry point. The benchmark
reports arithmetic mean per-block cost derived from throughput, not p95 fault
latency. It uses host dynamic-library SIMD optimizations and cache-hot buffers;
these rates must not be copied into a guest/kernel capacity estimate. No codec
format is committed for filesystem storage by this screening result.


## Explicit pageout/refault checkpoint

`opt/page-store-device=<device>` separately enables the experimental backing
for this boot. Without this recognized boot-selected image, syscall `0xB40`
returns EOPNOTSUPP and does not admit any mappings. With it, initial-namespace
CAP_SYS_ADMIN is required. Operations are scalar: 0 maps a new RW/NX private
region (aligned length, at most 16 MiB), 1 explicitly pages out one address,
2 returns global stored slots, 3 samples free physical frames, and 4 selects a
one-shot test failure (0 clear, 1 write error, 2 read error, 3 bad read checksum,
4 short write, 5 short read, 6 write error plus rejected rollback publication).
Failure controls are experimental diagnostics, not a proposed production ABI.

A sticky AS admission interlock excludes CLONE_VM while the experiment is active
and rejects admission for an AS that has ever shared task execution. Fork is
checked under its source mapping gate. These restrictions last until exec,
even after all pageable regions are unmapped or an opted-in allocation fails.
The dedicated mapping kind cannot be mprotected or remapped; unmap/MAP_FIXED
must cover its entire range, preventing unreachable slots from remaining with
surviving fragments. Other mapping kinds retain their existing behavior.

Pageout reserves its slot before PTE mutation, takes an extra frame reference,
detaches under AS serialization, and waits for full TLB ACK before disk writes.
The backing retains the slot through I/O and revalidation. A failed write restores
the resident PTE; if publication also fails, the backing owns a recovery frame
that a later fault can republish. Successful pageout drops the retained frame,
allowing the independent PMM quarantine/barrier worker to return it to allocation.
Fault-in prepares its frame and reads outside AS/PT locks, checks the immutable
slot checksum, and revalidates the captured mapping before publication. Bad
reads never turn into demand-zero data. Entire-region and AS teardown release
slot and recovery references after normal mapping retirement.

This is still **not automatic reclaim or arbitrary-app paging**. Existing raw
kernel user-pointer consumers lack scoped eviction pins. For the opted-in AS,
an ordinary syscall therefore restores every pageable leaf before entering
handler locks. Scalar paging, numeric signal/exit and restricted mapping calls
are exemptions. Kernel-origin page faults in an opted-in AS never initiate
paging I/O; safe usercopy retries explicitly enter the user-fault resolver.
The conservative barrier loses paging benefits across ordinary syscalls and
can affect unrelated lazy-file raw accesses in this diagnostic AS. It must be
replaced with real access exclusion/conversion before production pressure-driven
reclaim or threaded apps. VirtIO I/O also still uses its existing polling backend.
No reserved fault-in frame or background paging worker is claimed yet.

The static musl fixture `scripts/workloads/pageable_memory.c` verifies 128 unique
page patterns through real user #PF refaults, six I/O/recovery failures, raw
syscall output, evicted signal-altstack construction/return, rejection of fork
and new threads, whole-region teardown, normal process exit with stored pages,
previously-shared-AS rejection and non-admin denial. A native eight-vCPU run
reports free frames 2012643 → 2012777 after 128 pageouts and a userspace wait for
quarantine drain (net +134 includes background activity, not exactly 128 isolated
frames). The stored count is 128 before refault and zero after the suite.
Entry/scheduler compiled audits and targeted host store/admission/range/usercopy
tests pass. Native logs are under `build/paging-compression-20261028/native/`.


Additional native controls pass: all 1024 backing slots fill; the next pageout
returns ENOSPC without disturbing its resident bytes; refault releases one slot
and permits another pageout; whole-region unmap returns the store to zero. The
same exact kernel booted without the option returns EOPNOTSUPP for the private
ABI and still creates/joins a normal pthread. Both configurations shut down with
checked guest sync and original QEMU owner exit 0. The test image and server are
private artifacts; no backing disk or codec corpus is part of Git.

Next priority is scoped kernel-user access exclusion (and fault-in reserves),
not enabling this eager syscall-entry fallback for existing applications. Then
add real pressure-driven victim selection with bounded worker I/O, quantify
mixed-workload throughput/latency, and only then add compressed-RAM storage.
