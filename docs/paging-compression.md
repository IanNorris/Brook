# Paging and compression — staged implementation

Branch: `wip/paging-compression-20261028`. Goal: reduce memory-pressure failures
and improve mixed-workload throughput without trading them for corrupt mappings
or unbounded reclaim stalls. This records an opt-in experimental implementation;
general-purpose application paging is not enabled.

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
kernel user-pointer consumers need scoped eviction exclusion. The initial
checkpoint restored every pageable leaf before ordinary syscall handler locks;
read/write now use the lexical leases below and restore only their buffers.
Unaudited syscalls retain the eager fallback. Scalar paging, numeric signal/exit
and restricted mapping calls are additional exemptions. Kernel-origin page faults in an opted-in AS never initiate
paging I/O; safe usercopy retries explicitly enter the user-fault resolver.
The conservative barrier loses paging benefits across ordinary syscalls and
can affect unrelated lazy-file raw accesses in this diagnostic AS. It must be
replaced with real access exclusion/conversion before production pressure-driven
reclaim or threaded apps. VirtIO I/O also still uses its existing polling backend.
The initial checkpoint had no fault-in reserve; the bounded reserve below now
covers data-frame allocation failure. No background paging worker is claimed yet.

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


## Lexical read/write eviction exclusion

`UserAccessScope` retains the original AS and cancellation protection for each
returning read/write handler. A per-process scope chain supports nesting; each
stack node is also visible in the AS registry until it unlinks. Process copies
clear inherited scope pointers. Coverage is registered under AS mutation before
prefaulting, and PageOut checks that same registry before detaching. There is no
allocation for the registry or its ranges: up to 32 disjoint intervals are
represented inline, with overflow conservatively covering the entire AS rather
than rejecting valid I/O. Adjacent/overlapping intervals coalesce. Empty/invalid
intervals have defined behavior; failed validation may conservatively retain
coverage until scope exit. A pointer validated by an inner scope must not escape
that scope for later raw access.

The audited read/write syscall entry points bypass eager preparation. Their
existing UserBuffer checks now add the relevant range to the current scope.
Read output admission uses UserBufferWritable, resolving CoW and checking write
permissions before backend locks. Kernel-buffer internal write helpers remain
separate; they must not register trusted bounce buffers as user addresses.

This is eviction exclusion, **not a physical/DMA pin or protection from raced
munmap/mprotect/CoW sharing**. CLONE_VM remains forbidden and the broad syscall
fallback remains for unaudited consumers. Leases are deliberately lexical in
returning handlers, not dispatcher-wide: terminal exit/failed-exec paths may not
unwind ordinary C++ frames. Future broader admission must handle those paths and
all kernel user-memory consumers explicitly.

Host tests exercise coalescing, overflow-to-whole coverage, bounds, nesting,
removal and publication/victim-check serialization while a lease owner waits.
Private diagnostic operations 5 (validate a page under a real scope and attempt
pageout) and 6 (current AS lease count) verify exclusion and cleanup in the guest.
The static musl `pageable_io_leases.c` fixture passes both a Linux writable/COW
oracle and Brook native tests: stdout leaves all 16 cold pages stored, a targeted
file read restores only its destination, and a targeted write restores only its
source. Invalid FD/buffer, nonblocking EAGAIN and signal-interrupted blocking
read paths leave zero leases. A covered page returns EBUSY; it can be paged out
after scope unwind. Original integrity/failure/signal/exhaustion/exit tests also
pass on the lease build. Artifacts are in `build/paging-compression-20261028/`,
including `lease-build.log`, `lease-linux.log` and `native/leases-result.json`.
No high-pressure throughput claim is made from this functional test.


## Fault-in reserve and heap-independent one-page bookkeeping

Single-page MappingTransaction retirement, fresh-page and backing records now
have zero-initialized inline storage. Larger requests still allocate and retain
the original partial-failure cleanup. A host test compiles the real transaction
code with heap allocation denied and verifies that one-page preparation succeeds
without an allocation, heap-owned partial requests unwind, and no inline address
is passed to kfree.

An explicitly enabled backing device also reserves eight ordinary PMM frames
(32 KiB) at boot, owned as KernelData by KernelPid. If normal data-frame allocation
fails, checkout transfers a sole frame to User accounting for the destination
allocation PID. The checkout retains one reference and stages an additional one
in the mapping transaction. Failed I/O, checksum or publication destroys the
transaction first, then returns the unmapped sole reference to the reserve.
Successful publication drops the checkout reference, retaining the PTE reference.
An outer fault lifetime holds AS/cancellation protection through all that cleanup.

Successful pageout may replenish the reserve only after detach/TLB acknowledgement
and successful storage. It transfers the retained frame directly, rather than
freeing and hoping to allocate again. Failed-write rollback/Recovery frames never
replenish the pool. A full reserve releases excess frames normally. Reserve and
PMM locks are separate and neither covers I/O. PmmReclassifyPrivateFrame requires
aligned, live, sole-reference, unmapped User/KernelData frames and updates tag/PID
lists under the PMM lock; it does not grant ownership from a raw physical address
or constitute an eviction pin. Host tests reject shared/mapped/freed/wrong-tag/
misaligned transfers and verify ownership-list changes.

Existing detached leaves retain intermediate page tables. Together with inline
transaction/gate storage and resident slot metadata, this removes heap/page-table
allocation from the validated dedicated-region fault-in path. The current VirtIO
read/write request path uses its boot-allocated request/DMA/cache storage. This
is not a guarantee for arbitrary mappings, backends or every global OOM state.
An empty reserve returns OutOfMemory; it never waits holding AS/store locks.
Reserve refill retains RAM, so reserve occupancy is separate from PMM free pages.

Private diagnostic operation 7 reports available reserve frames; operation 8
forces at most 32 subsequent ordinary pageable data-frame allocation failures.
Operation-4 mode 7 rejects fault-in publication after preparation. These remain
boot-opted-in, admin-only test controls. scripts/workloads/pageable_reserve.c
passes in Brook: successful fallback, failed/short/corrupt reads and publication
restoring reserve occupancy, exhaustion returning ENOMEM without losing stored
pages, progress after a resident victim supplies a frame, and eight reuse cycles
ending with zero slots and all eight reserve frames. Host PMM/reference tests,
transaction storage tests and the earlier paging/I/O native suite also pass.
Native evidence: build/paging-compression-20261028/native/reserve-result.json.
The tests inject data-frame allocation failure; they are not a system-wide memory
pressure benchmark and do not yet demonstrate automatic victim selection.

Next: bounded victim selection with correct exclusion for its caller context,
then background I/O where useful and actual pressure/latency controls. In
particular, reclaim must not evict earlier pages behind the eager preparation
cursor of an unaudited syscall. That fallback needs whole-AS admission/exclusion
or explicit no-reclaim behavior before any concurrent/background pager is enabled.
Compressed RAM and indexed immutable-file compression remain later stages.

## Synchronous victim-reclaim checkpoint

Private operation 9 enables/disables victim selection for the current exclusive
experimental address space; it defaults off. Only after ordinary frame allocation
and the eight-frame reserve both fail may a permitted fault attempt reclamation.
An explicit fault-origin argument identifies user-instruction faults. Safe
usercopy additionally requires a live current-AS lease covering the fault page;
a synthetic user error bit alone never grants reclaim permission. Eager
preparation for unaudited syscalls and remote cleanup leave reclaim disabled,
so they cannot evict earlier pages behind their preparation cursor.

Selection is round robin within that same address space and dedicated mapping
kind. It excludes the incoming page, all leased pages, nonresident/Recovery
entries, CoW leaves and multiply referenced/mapped frames. The authoritative
PageOut checks run again after selection unlocks. An AVL successor query skips
holes and ordinary extents without a linear restart. One traversal visits each
page/extent at most once, releases the AS gate and yields after at most 64
visits, and attempts at most one victim write. The whole traversal is bounded
by the admitted address space's current mappings, not a fixed 64-page limit.
There is no worker or concurrent/background eviction.

Successful victim storage transfers its frame into the reserve and the incoming
fault checks it out directly. Victim detach changes the AS generation: the fault
must recapture the same target mapping identity/backing/page index and OnDisk
slot before normal final generation validation. Incoming stored bytes survive
victim write failures; those are resource failures, not incoming-data corruption.
User-origin nonpresent faults enable interrupts only across their sleeping
resolver and restore CLI before exception bookkeeping. Reclaim refuses an
interrupt-disabled caller; yields restore IF in this explicitly admitted path.
Private operation 10 directly exercises one selection attempt for diagnostics.

This is forward progress for the validated single-page fault scenarios, not a
proof that arbitrary multi-page instructions or an oversized working set can
progress under global OOM. General app paging, CoW/fork/shared-AS eviction and
memory-pressure performance acceptance remain unfinished.

Validation on the native KVM/VirtIO backing: scripts/workloads/pageable_reclaim.c
passes genuine user-fault and scoped-write reclaim after injected allocation and
reserve exhaustion, all-victim lease exclusion, unaudited eager no-reclaim,
victim EIO/short-write/Recovery preservation, and three byte-checked cycling
rounds. Its --scan case crosses the 64-visit yield boundary and performs a full
wrapped all-leased traversal; --full-store preserves the resident victim on
ENOSPC. Existing 128-page/failure/signal, reserve and read/write lease fixtures
also pass. Eight relevant host binaries and both compiled entry/scheduler audits
pass. Evidence is in build/paging-compression-20261028/native/reclaim-*.json;
this kernel's VM completed checked sync and QEMU exit 0. These are deterministic
allocation-failure tests, not measured system-wide pressure improvement.

## Lazy zero-fill and soft resident target

Private operation 11 maps the same bounded/exclusive dedicated regions lazily.
An untouched entry is explicitly Zero: no data frame and no disk slot. Mapping
creation prepares intermediate page tables even with zero prepared data pages,
so later faults need no table allocation. A host test uses the real mapping/VMM
code to publish first/last leaves with allocation denied and verifies failed
skeleton creation publishes no range. Zero faults clear all 4096 bytes before
publication. Failed preparation/publication leaves Zero unchanged; Recovery
still republishes its retained good bytes and is never inferred to be zero.

Operation 12 sets a soft per-AS held-frame target (pages, zero disables); victim
selection still requires operation 9. Before an eligible fault allocates above
the target it attempts bounded reclaim, excluding eight recent fault addresses
as well as the incoming page and all leases. No eligible victim, a full store
or a victim write error permits ordinary allocation above the target rather than
forcing thrash or rejecting an otherwise satisfiable fault. Hard allocation
failure remains a separate emergency path; there is at most one victim I/O
attempt per fault. Failed victim I/O may detach/restore and change generation,
so even that attempt requires target snapshot refresh. Recent exclusion and a
soft target improve local progress but are not an arbitrary-instruction OOM proof.

Operation 13 reports held frames (selector 0), Zero pages (1), stored pages (2),
Recovery pages (3) or Evicting pages (4) within current dedicated regions. Held
frames include backing-owned Recovery/in-flight source frames until successful
storage, not only present leaves; reserve, metadata, page tables and transient
fault-in frames are additional memory. These are admin-only experimental
snapshots, not production telemetry. Unmapped-but-not-yet-retired frames may
outlive removal from the current range census.

clock_gettime now has a lexical output-buffer lease and skips experiment eager
preparation. Its local kernel value is copied after clock/scheduler reads release
their locks. This permits timing without restoring unrelated cold pages. The
native fixture verifies a pageable timespec restores only its page and leaves
other stored pages cold, and checks zero lease count on return.

scripts/workloads/pageable_working_set.c validates eight sparse full-page zero
reads allocating only eight frames out of a 1024-page region, with no disk writes.
It forces ordinary allocation failure and publication rejection on a Zero fault
and checks reserve restoration. Soft reclaim write/short-write/rollback failures
preserve victim bytes, retain Recovery correctly, publish the incoming zero page
and account for overshoot. A full-region read lease prefaults safely beyond the
target and consumes only its actual file bytes; no victim is evicted behind it.

Two native runs, with reversed eager/soft order, use the same 8-vCPU/8-GiB KVM VM
and 4-MiB backing. A 4-MiB dataset under a 64-page target holds 256 KiB of data
frames and stores 3.75 MiB (93.75% less mapped data RAM); eager mode holds all
4 MiB. This excludes metadata, reserve and table memory. Filling the soft region
took 40.5/63.5 ms versus eager 0.92/0.95 ms; scanning and verifying every byte
took 56.7/71.5 ms versus 5.33/6.25 ms. Per-page verification p95 was 111.8/82.6 us
versus 4.55/10.49 us; maxima 6.50/8.30 ms versus 29.6/47.7 us. These timings include
4096-byte validation and clock overhead, not isolated fault latency. Two ordered
samples on host-cached raw backing are not a statistically controlled SSD or
system-wide memory-pressure benchmark. Paging saves RAM while adding cold-I/O
latency; no throughput improvement over an unconstrained resident set is claimed.

All prior native core/reserve/lease/reclaim/long-scan/full-store fixtures pass on
this kernel. Nine host binaries including actual VMM/AS ownership tests pass;
an older VMM heap-failure test now requests multi-record allocations because
single-record preparation intentionally uses inline storage. Compiled entry and
scheduler audits pass. Evidence: build/paging-compression-20261028/native/
working-set-*.json and working-set-{host,entry-audit,start-audit} files. The VM
completed checked sync and original owner exit 0. Next acceptance needs actual
system-pressure/response measurements; compressed RAM can then address the cold
page latency demonstrated here. General application paging remains disabled.

## Actual constrained-memory comparison

scripts/workloads/pageable_pressure.c compares a 256 MiB dataset on fresh private
512 MiB, eight-vCPU KVM overlays of the identical accepted seed and exact kernel.
Both attach 256 MiB raw swap (65536 slots) and hold 128 MiB of ordinary anonymous
RAM. The pressure is bounded to this guest; there is no host stress process.
Both begin with 58626 free frames and have 25799 after the ordinary allocation.

Eager mode maps 96 MiB of the dataset, then returns ENOMEM. The serial log confirms
an actual PMM failure with free_pages=0, not injected failure or a synthetic
resident cap. Its partial mapping cleanup completes and the VM syncs/shuts down
with original QEMU owner exit 0. A fresh-overlay paged run completes the entire
256 MiB dataset, validates every byte after writing and again after faulting from
disk, preserves the ordinary allocation bytes, and ends with zero stored slots.
It holds 4096 data frames (16 MiB), stores 61440 pages (240 MiB), and samples a
minimum 21319 physical free frames (~83.3 MiB, ~83.7% of guest RAM in use). Reserve
occupancy is seven, separate from data-frame accounting. Filling took 3.552 s and
full verification 4.092 s; sampled 4 KiB verification p50/p95/max were
80.9 us /124.0 us /40.85 ms. These include verification and clock overhead.

This proves a larger useful working set and successful cleanup under a real RAM
limit in the restricted dedicated allocator. It is not ordinary browser paging,
a desktop responsiveness result, or a statistically controlled disk-speed claim.
Raw backing was host cached. Inputs, hashes, launch parameters and native result
JSON are retained in build/paging-compression-20261028/pressure-512/. Both variants
used the same frozen seed via fresh qcow2 overlays; the control overlay remains
preserved as eager.qcow2. The original user disks were never used.

## Bounded compressed RAM tier

The existing PageStore token/refcount table now distinguishes immutable RAM and
raw-disk payloads. A recognized backing can opt in with
`opt/page-store-ram-mib=1..16` (absent/zero disables). Brook already vendors LZ4
1.10.0 for panic capture; paging reuses that freestanding, heap-disabled source
and its scalar build. No new codec source or dependency was added. Official
API contracts: https://github.com/lz4/lz4/blob/v1.10.0/lib/lz4.h

At boot the tier allocates its arena, bitmap, external LZ4 state and separate
4 KiB encoding/spill scratch buffers. Those remain resident. The bitmap arena
allocates contiguous 64-byte units using finite first fit, skipping full words.
It neither allocates search records nor waits for defragmentation. Offset and
exact-length pairs are private trusted slot metadata, not public generation
identities or independently validated allocation handles. Caller serialization
and exact release lengths remain requirements. Compression is accepted only at
3072 bytes or less (at least 25% savings after allocation rounding). An
incompressible page, encoder failure or fragmented/full arena falls directly
back to the existing raw disk write; it does not retry or spill another entry.
The configured logical slot limit still applies to both tiers together.

RAM publication occurs only after compression/allocation/copy succeed. Load
requires exact 4096-byte safe decoding and the original uncompressed checksum.
Failed decode/checksum leaves slot ownership intact and output unpublished.
Retained tokens continue to own one immutable payload; last release frees its
arena allocation exactly once. Explicit Spill preserves token, references and
checksum: decode into resident scratch, write the slot's fixed raw location,
then switch to disk and release RAM only on full success. Failed/short writes
leave RAM authoritative. There is no automatic compressed-entry replacement or
background spill policy yet, nor return of empty arena pages to PMM.

Private control 14 selectors: 0 arena capacity, 1 rounded allocated bytes,
2 payload bytes, 3 RAM slots, 4 disk slots, 5 compression fallbacks, 6 requested
resident allocation bytes (arena+bitmap+workspace+scratch/context), 7 slot-record
bytes, 8 successful spills, 9 failed spills, 10 decode errors, 11 checksum errors,
12/13 successful disk writes/reads, 14/15 RAM stores/loads. Requested bytes exclude
heap allocation headers/rounding/expansion overhead; the boot diagnostic's
resident-byte value has that same limitation. Region metadata, page tables and
fault reserve are additional. PageStore writes/reads count logical stores/loads;
diskWrites additionally includes successful spills, so these are not interchangeable
counters or a count of failed/partial physical I/O. Private control 15 spills a
stored page addressed in the caller's dedicated mapping: pin backing, drop AS
mutation, retain token, perform spill, release pin outside AS mutation. Dedicated
mapping state is now named Stored because its payload tier is opaque to PTE code.

Host tests cover 30000 allocation operations against an independent fragmentation
oracle, deterministic sufficient-free-but-fragmented rejection, exact 3072/3073
thresholds, real freestanding LZ4, incompressible/failed-encoder fallback,
malformed/corrupt encoded bytes, failed/short spills, retained identity and
1000 mixed reuse cycles. ASan+UBSan of real LZ4/store/pool tests passes. All eleven
relevant host tests, both image-tool tests and compiled entry/scheduler audits
pass. Native pageable_compressed.c verifies RAM/disk/refault integrity, failed
and short spills, disk read/checksum/publication failure reserve cleanup, forced
allocation-failure reclamation, and zero slots/arena allocations at teardown.
Native raw core/reserve/lease/reclaim/long-scan regressions also pass with the
tier disabled. Both native VMs complete checked sync and original owner exit 0.

A first synthetic 16 MiB comparison uses the same kernel, fresh overlays of the
same seed, 8 CPUs/8 GiB and 4096 logical slots. The compressed variant allows
256 resident data pages (1 MiB) plus a fixed 4 MiB arena and 32856 bytes requested
for bitmap/context/workspace/scratch: 5275736 bytes. The raw variant allows
1280 data pages: 5242880 bytes (about 0.63% less). Common slot/region metadata,
page tables and fault reserves are additional in both. The arena's full capacity,
not merely its used payload, is charged to the compressed variant.

Both write then verify all bytes. Compressed fill/scan: 42.6/87.0 ms, verification
p50/p95/max: 18.7/23.3/85.0 us; all 3840 stored pages fit RAM, with zero disk I/O.
Raw fill/scan: 120.9/246.5 ms, p50/p95/max: 34.4/113.1/7994.8 us; 2816 pages remain
stored on disk, with 6783 successful disk writes and 3967 reads during timed
phases. Thus this first ordered sample is about 2.8x faster at a similar requested
resident budget; it is not a statistically controlled result or a browser claim.
Per-page timings include 4096-byte verification and clock overhead. The data is
intentionally compressible; native pressure/full-pool and broader corpora remain
necessary. The separate default 4 MiB lease/clock regression also passes with
compression enabled. Its fixed 4 MiB arena exceeds the saved data footprint, so
that regression's timing must not be advertised as a total-memory improvement.

Evidence and frozen hashes/parameters: build/paging-compression-20261028/
compressed-native/{metadata,result,measure,raw-measure,lease-regression}.json,
compressed-{all-host,asan,image-tools} logs and entry/start audit JSON. The
pressure-control seed was preserved; compression used a separate private seed.
Next: constrained-memory mixed-tier/full-pool measurements, then indexed
immutable compressed storage and host inspection/extraction tools. General
application paging and automatic reclaim of arbitrary shared/forked ASes remain
unfinished and disabled; no completion of the overall goal is claimed here.

## Full compressed-arena pressure acceptance

On the tier-capable kernel in a fresh 512 MiB/eight-vCPU VM, the 256 MiB fixture
holds the same 128 MiB ordinary allocation and a 16 MiB data target, plus the
fixed 4 MiB arena/workspace overhead. It begins with 57326 free frames and has
24499 after the ordinary allocation. The complete fill and byte-checked refault
scan passes: 9361 stored pages in RAM and 52079 on disk, with arena allocation
4193728/4194304 bytes, payload 3603985 bytes and 104158 disk fallbacks. The pool
is effectively full and fallback is exercised repeatedly. Minimum sampled free
frames are 20019 (~78.2 MiB). Fill/scan took 2.514/3.219 s, verification p95
56.1 us and maximum 6.94 ms. This is one functional mixed-tier pressure run, not
a controlled performance comparison with the earlier raw-only kernel/budget.
Ordinary allocation bytes remain intact; teardown leaves zero slots and zero
arena allocation/payload. Checked sync and original QEMU owner exit 0 pass.
Evidence: build/paging-compression-20261028/compressed-pressure-512/{result,
metadata}.json. The pressure fixture now verifies and reports both tier counts,
capacity/fallbacks and pool cleanup explicitly.

The next disk stage has a shared validated indexed reader and deterministic host
build/list/inspect/verify/extract/tar-export tools. Its exact layout, limits,
extraction contract and outstanding native integration gates are documented in
[compressed-image-format.md](compressed-image-format.md). That host-format checkpoint did not yet include a VFS adapter; the following
checkpoint adds native integration without claiming whole-goal completion.


## Read-only compressed filesystem and reclaimable file cache

The opt-in `brookimg` adapter now mounts indexed images, including at `/nix/store`.
See [compressed-image-format.md](compressed-image-format.md) for flags, exact
format, host tools and restrictions. A native static ELF verifies cross-block
reads, hardlinks, symlinks, openat, permission denial, private CoW, shared-write
rejection and EROFS mutations. A detached mount remains usable through retained
handles. A real 14-path Bash 5.3p9/jq 1.8.1/coreutils 9.10 closure runs as uid 1000
in a 512 MiB/eight-vCPU VM. Its 53,670,986 unique file bytes occupy a 28,631,040-
byte indexed image. Bridge job round trips are not cold-launch benchmarks.

The immutable file cache no longer retains vnodes. Never-reused namespaces avoid
stale identity reuse while allowing detached mount metadata to be freed. Entry
size falls from 32 to 24 bytes. A bounded pressure worker reclaims cache-only,
unmapped frames below 64 MiB free, with 128 MiB hysteresis and 256-entry scan
batches. A pressure fixture primes a mapped libc, unmaps it, holds 184 MiB of
ordinary allocations and observes 86 cache entries retired, with 69 free pages
recovered by its sample. Pinned/mapped frames remain excluded; allocation and
retirement timing mean this is partial progress, not guaranteed OOM prevention.
The same Nix jq binary executes correctly after reclaim.

Private experiment control 16 returns cached/scanned/reclaimed counters for
selectors 0/1/2. Control 17 temporarily pauses this worker for a controlled
pressure test and restores it afterward; it is not a normal application ABI.
Both controls require the existing root-only experimental page-store setup.

A native payload-corruption test found that legacy mmap read-ahead converted a
failed read into cached zeroes. Read-ahead now publishes only complete verified
file pages, retaining a valid prefix when later data fails. Demand reads also
reject short in-file data. The check covers read EIO, rejected mapped data and
rejected execution, with the kernel surviving and shutting down cleanly.

Host coverage includes all 18 relevant filesystem/cache/paging tests, Python
image-tool tests and adapter/cache ASan+UBSan. Native evidence lives in
`build/paging-compression-20261028/compressed-{nix-native,corrupt-native}`.
The next useful acceptance step is controlled raw-versus-compressed cold-load
throughput and resident-memory measurements, followed by larger workloads.
General anonymous application paging remains restricted to the private dedicated
mapping API; safe arbitrary threaded/forked application paging, dynamic compressed
arena return/spill policy and wider workload acceptance remain unfinished.

## Controlled file-loading comparison and contiguous device reads

The native byte-verifying workload `scripts/workloads/compressed_image_bench.c`
loads all 1071 unique regular files (53,670,986 bytes) in the same Nix closure.
Its companion `scripts/compressed_image_bench_manifest.py` generates exact guest
paths, sizes and FNV checksums from verified image bytes without opening image
paths on the host. Build the probe with `$MUSL_CC -O2 -static`; generate a
manifest with `python3 scripts/compressed_image_bench_manifest.py IMAGE MANIFEST
--prefix /nix/store --lz4-library build/host-tools/libbrook-image-lz4.so`. Put
both in a private initrd, then run `PROBE MANIFEST read|map HOG_MIB` as root in
the experimental page-store VM. The optional ordinary allocation is bounded at
192 MiB and verified afterward. Each file's full contents are checked. Guest
CLOCK_MONOTONIC timings include opens, stat, read/mmap, hashing and cleanup.

The comparison uses fresh 512 MiB/eight-vCPU VMs, two boots per mode/codec with
reversed codec order, and two subsequent passes per boot. Host caches are warm;
this is guest-cold loading, not physical SSD-cold loading. QMP packed-device
read bytes/requests bracket the whole bridge job, including helper setup outside
the guest's timed interval. This small sample does not establish browser/game
launch speed. Holding 128 MiB stays above the cache-worker threshold in this
fixture; holding 160 MiB activates reclaim and completes all byte checks.

Before further tuning, median guest-cold read passes take 394 ms raw versus
532 ms LZ4, with 55.43 versus 27.73 MiB of device reads. Mapped passes take
537 versus 661 ms, with 66.01 versus 28.04 MiB read. Compression reduces I/O but
is not yet a cold-load throughput win. Both variants retain decompressed cached
file pages; compression does not itself shrink that resident working set.

The device trace identified adjacent 4 KiB cache misses issued as separate
requests. The Virtio read path now coalesces only a completely contiguous set of
2..16 misses into the existing request-guarded DMA buffer. Holey sets keep the
parallel-slot path. The transfer completes successfully before cache publication;
SubmitRequest alone owns notification, completion validation and ring advancement.
There is no new buffer. The actual persistent DMA allocation is 256 KiB; corrected
comments previously said 64 KiB. Driver read failures now return EIO rather than
EPERM, matching DeviceOps' negative-errno contract.

With coalescing, cold read requests fall from 4678 to 883 raw and 6709 to 444
LZ4; mapped requests fall from 4846 to 1051 raw and 6714 to 449 LZ4. Device bytes
are unchanged. Median read times are 413/557 ms raw/LZ4 and mapped times 487/619
ms, so the request reduction must not be advertised as a proportional loading
speedup. Under 160 MiB ordinary allocation, both variants continue to verify
all bytes while thousands of cache entries are reclaimed per pass. Cache pressure
results are noisy because asynchronous reclaim overlaps loading.

Acceptance includes bounded planner edge/hole/wrap tests, all 90 benchmark
passes, eight-worker 64 MiB read/write/boundary/small-file checks and 16384
concurrent read/mmap page checks followed by persistent verification and cleanup.
A private QEMU blkdebug rule injects one EIO into a selected compressed data
transfer: `compressed_image_read_retry.c` observes EIO then verifies the full
64 KiB decoded block on retry. No failed DMA bytes are cached. The final VM
completes checked sync and original owner exit zero. This tests device errors;
it does not simulate a completion timeout or prove every reset path.
QEMU injection configuration follows its
[blkdebug documentation](https://www.qemu.org/docs/master/devel/testing/blkdebug.html).

Evidence: `build/paging-compression-20261028/compressed-image-benchmark/`, including
comparison/metadata JSON, per-VM jobs/QMP counters and shutdown receipts. A
three-second CPU capture during six verified read passes is retained in
`final-native/read.profile`: 2345 CPU samples, no dropped events, 276 samples
belonging to the six probe processes. Of these, 97 land in memcpy called from
LZ4, 68 in device SubmitRequest, 30 in the workload's hashing loop, 28 in
LoadBlock and 16 in Read. The freestanding codec currently maps constant-size
LZ4 copies to out-of-line rep-movsb calls; measuring explicit compiler builtins
for these small scalar copies is the next concrete CPU lead. These are samples,
not an exact wall-time attribution or a reason to weaken checksum validation.

## Scalar codec copies: measured cold-load improvement

The CPU lead above was confirmed in generated code. Explicitly selecting
`LZ4_memcpy=__builtin_memcpy` removes 27 out-of-line memcpy call sites from
LZ4_decompress_safe. The kernel's existing `-mgeneral-regs-only -mno-sse
-mno-sse2` restrictions remain in force; inspection of the complete kernel LZ4
object finds no SIMD/FP instructions. Variable-size lowering can still use the
existing runtime, and LZ4_memmove remains unchanged. No heap, lock or workspace
is added. This also follows the vendored codec's own freestanding guidance.

Repeated short match offsets 1..32 now have explicit tests at sizes from zero
to 64 KiB, with exact output capacities, one-byte-short capacities, truncated
inputs and untouched guard regions. The image, VFS and compressed store tests
pass with the new definition. ASan+UBSan also pass with the actual freestanding
codec compiled using -fno-builtin and the explicit builtin override.

Another two fresh boots per mode/codec, with reversed codec order, verify all
1071 files and all 53,670,986 bytes on each pass. Median guest-cold read time is
320 ms LZ4 versus 375 ms raw. Against the preceding coalesced LZ4 build's 557 ms,
that is about 43% less elapsed time. Guest-cold mapped loading is 385 ms LZ4
versus 503 ms raw. With 160 MiB of extra ordinary allocations and active cache
reclaim it is 373 ms LZ4 versus 486 ms raw (~23% less); subsequent pressure passes
continue to verify all bytes while thousands of cache frames are retired.
Device bytes remain about half of raw. These are byte-hashed fixture load times,
not app launch timings, and host caches remain warm. Two runs per case do not
establish broad workload performance. Decoded resident file pages occupy the
same amount of RAM in both formats.

The 36 additional image passes, generated-instruction audit, sanitizer logs,
exact kernel hashes and per-VM receipts are retained under
`compressed-image-benchmark/builtin*`. Final native acceptance enables the 4 MiB
compressed-RAM arena and repeats explicit RAM/disk spill/read/publication failure
checks, reserve reclamation and pool cleanup, plus the 256 MiB mixed-tier pressure
fixture. The injected packed-image EIO/retry check is repeated on the same build.
General application paging remains experimental; broader real workload
acceptance and pressure responsiveness/latency gates remain open.

An additional sequential check found a pre-existing reserve policy gap: the
pressure fixture ends with seven pooled emergency frames, and the next fresh
compressed-store probe (which expects eight at entry) fails that precondition.
Slots and compressed-pool allocations are zero. Healthy soft-target turnover
currently returns the victim to an already-full reserve, frees it to ordinary
PMM, then checks out another reserve frame for fault-in. This is reserve
consumption, not a leaked physical frame. Next: transfer a successfully stored,
TLB-retired same-AS victim directly to its fault transaction, then address
bounded healthy refill for genuine emergency consumption. The sequential failure
is retained as `builtin-accept/after-pressure-reserve.json`; no full reserve-
recovery or whole-goal completion is claimed by this codec checkpoint.

## Fault-driven victim handoff and sequential reserve retention

Successful same-AS fault-driven reclaim now transfers its retained victim frame
directly to the fault transaction after storage and TLB acknowledgement. Explicit
pageout still returns detached frames to the reserve. The incoming transaction
stages the handed-off reference before fallible snapshot refresh; cancellation,
failed decode and rejected publication retire it once, while both stored copies
remain authoritative. Existing Recovery frames do not enter this reclaim branch.
No new allocation, buffer, refcount-based eviction rule or lock across I/O is
introduced. This fixes the healthy soft-turnover reserve consumption above.

The pressure fixture now asserts all eight reserve frames are available both
during its steady state and after cleanup. A new `pageable_handoff.c` regression
exhausts the reserve, forces allocation failure, verifies successful direct
reuse, then rejects target publication after a successful victim write through
a leased user-copy operation. It checks both stored byte copies, released leases,
subsequent recovery and explicit reserve replenishment. Raw and compressed
variants are tested separately. Genuine emergency borrowing can still transfer
reserve capacity into resident PTEs; bounded healthy-memory refill of that case
remains open rather than being inferred from ordinary unmap/quarantine.

Use `nix-shell --run 'scripts/build_paging_probes.sh'` to rebuild every paging
probe from the current source. In particular, pageable_memory, pageable_reserve
and pageable_reclaim inject raw device callbacks and require the RAM tier off;
pageable_compressed explicitly spills RAM payloads before testing disk failures.
The working-set probe selects its raw-only failure expectations at runtime.
Do not reuse pre-compression fixture binaries with a RAM-enabled kernel.
Evidence: `compressed-image-benchmark/handoff-current-compressed-v2/` and
`handoff-current-raw/`, with the exact kernel/probe hashes in handoff-metadata.json.
