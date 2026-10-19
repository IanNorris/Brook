# Coherent regular-file mappings

Current checkpoint (19 October): writable regular-file MAP_SHARED is enabled
for filesystems with canonical retained FileObject backing, currently ext2.
Mapped truncate still returns EBUSY. The older stages below document the
foundation and remaining broader compatibility/error-injection work.

ABI 29 retains a vnode identity in each file mapping descriptor. Fault
publication and mprotect upgrades mark canonical pages exposed before granting
write access, including PROT_NONE restoration. Noncanonical shared publication
fails, readonly descriptors cannot gain shared write access, and private copies
remain isolated. Extending ordinary writes zero cached old EOF gaps before
overlaying their confirmed payload.

`msync` snapshots and retains each intersecting shared-file descriptor, then
releases AS/file gates before range writeback and backend durability. It checks
alignment/flags, reports holes and backend errors, and ignores private/anonymous
data. ASYNC and zero flags currently perform the stronger synchronous flush.
The five-second worker flushes pending backing; shutdown quiesces it before
closing device writes and doing its final sync. Conservative exposure may
rewrite unchanged writable pages. This is not dirty-bit-based eviction.

Actual-module tests cover canonical publication, protection upgrades, ranges,
errors/retry, unmapping during sync, empty exposure and EOF-gap zeroing. All 52
host suites and both kernel entry audits pass. Six Wine-pattern plus fourteen
Linux-matched guest checks cover independent opens, read/write coherence, fork,
private isolation, readonly upgrades, SIGBUS past EOF and descriptor close.
Synced and initially unsynced pages retain their exact bytes and ownership on
a separate cold boot. Fixtures: `scripts/workloads/unix_rights_shared_file.py`
and `scripts/workloads/shared_file_mapping.py`. Evidence:
`build/workloads/wine-shared-mapping-20261019/candidate`.

## Current implementation and remaining gaps

Ext2 now embeds a `FileObject` in its existing retained live-inode holder.
Separate opens share that object; mappings retain a vnode and therefore its
inode holder. Quiescent last-holder/orphan disposal drops cached frames without
a sleeping lock or filesystem I/O. There is no permanent vnode/object cycle,
and a reused inode gets a fresh object. `VnodeOps::file_object` exposes the
borrowed object while a caller retains its vnode.

File faults now prepare canonical pages through that object outside AS mutation.
One per-inode gate serializes loading, so concurrent faults retain one completed
frame. Required bytes are read to completion across short progress; premature
zero/error is rejected without publication. Only the actual EOF tail is zeroed.
The fault profiler distinguishes new loads from hits.

VfsRead and VfsWrite now use the same gate and cached frames for ext2. Reads
currently fetch backend bytes and overlay resident pages. Successful ordinary
writes update only their written prefix in resident pages; errors preserve the
cached data. User buffers use bounded 64-KiB staging with copyin/copyout outside
file and filesystem locks. Unmapped truncate invalidates cached pages after any
backend resize attempt, including failures after mutation. Ordinary writes are
write-through. Conservative writable exposure, retained dirty backing and
snapshot writeback primitives are implemented, as are fsync/fdatasync/global
flush callers. Production mappings do not yet expose writable aliases; msync
and periodic flushing remain unimplemented. Later sections retain the historical
implementation evidence for these stages.

The older clean cache in `memory/file_page_cache.{h,cpp}` remains the fallback
for other filesystems. Both ext2 regular files and FAT still set the old cacheId
to zero; ext2 now uses its live-inode object instead. FAT has no file-object
accessor and does not acquire ext2's new coherence guarantees.
VfsTruncate still returns EBUSY while any file mapping pin exists. There is no
msync implementation. [Typed user-fault outcomes](file-fault-outcomes.md) now pass
baseline/candidate and deterministic stale-EOF checks as a prerequisite for mapped
resize; kernel raw-copy fault recovery remains separate.

Review also exposed PMM reference saturation at 255. A real-PMM hosted test
reproduced count 255 where 256 was required. Reference and mapping counts are now
32-bit, including the CoW consumer and provenance diagnostics, with explicit
overflow failures. Descriptors grow from 16 to 20 bytes (4 MiB extra at 4 GiB
physical RAM); the fixed provenance ring remains the same size. The actual PMM
passes 70,000 logical aliases and 16,384 concurrent pins/releases in the host
fixture; the VM passes 512 real framebuffer aliases. Neither proves coherent
regular-file sharing. Candidate evidence:
`build/workloads/file-cache-pmm-20261016/audit.json`.

## Required structure and ordering

The live-inode object, canonical loading, mapping registrations, publication
gates and retained writeback are implemented for ext2. Next connect mapped resize,
writable publication and msync. Its inode-holder references provide the lifetime
shared across opens. FAT's current directory-entry identity is valid
only while that entry remains allocated; rename/unlink/reuse require explicit
lifetime support before offering the complete contract on that filesystem.

GetPage returns a retained reference to the canonical winning frame, including
loading/error state. A shared mapping cannot fall back to an uncached candidate
when the cache is full. Private mappings initially read canonical pages and copy
on write. Ordinary reads and writes use the same object, with separate backend
I/O entrypoints to avoid recursive cache calls. Preserve partial write results.
Use bounded resident staging for usercopy outside file and AS locks.

The publication order is file gate, AS mutation mutex, then PT lock. Faults
prepare pages outside AS locks, then validate file and AS generations while
holding the publication gate. Generation checks without a common gate leave a
check/publication race with truncate. No filesystem I/O, usercopy, final backing
release or waiting for remote TLB acknowledgements under IRQ-disabled PT locks.

An initial dirty strategy may conservatively retain dirty state for every page
exposed through a writable shared alias. Serialized writeback takes resident
snapshots and reports backend errors; it must not clear dirty state merely
because a flush completed while a writable alias can still modify the page.
Wire range-aware msync, fsync/fdatasync and global sync into that path; arrange
background writeback. Dirty pages and their backing survive failed writeback.

Resize blocks old-size publication, invalidates affected aliases across registered
address spaces, completes TLB shootdown and only then retires frames/storage.
Partial EOF tails must be zero, and shrink/regrow must not reveal old data.
Mapping registrations need unregisterable tokens and protected AS reference
acquisition, avoiding a permanent file-object/AS reference cycle. Allocate
retirement jobs before destructive changes and retain them through cancellation.

## Acceptance

`tools/at-syscall-probe/filesharedcheck.c` is the initial Linux behavior reference.
It uses an exclusive temporary path, checks separate-open aliases across fork,
shared-store visibility to pread, pwrite visibility to mappings, private CoW,
msync/fsync plus reopen, partial EOF, whole-page SIGBUS, shrink/regrow zeroing,
and alias lifetime after unlink/close. It has a 60-second POSIX timer and child
cleanup. The host reference passes with static musl; evidence is in
`build/workloads/fileshared-linux-reference.log`. Reopen proves visibility, not
reboot persistence. It intentionally fails current Brook rather than accepting
ENOTSUP/EBUSY as success. It was also run against Brook and fails at the first writable shared mmap with
ENOTSUP (errno 95), the recorded unsupported baseline.

Add deterministic same-page concurrent-miss barriers, fault/truncate publication
races, writeback-error injection, concurrent redirty, close/unmap/exec cancellation,
and reboot persistence. The current probe's fork handshake does not establish
simultaneous same-page misses. FAT identity and unlink semantics need their own
acceptance; passing an ext2 test cannot establish FAT support. Renew both full
bootstrap paths and Chromium/Codium runs after the connected implementation.

Astra reviewed the current code and this architecture; the review did not claim
an implementation or acceptance result.


## Verified ordinary-I/O stage (2026-10-16)

`filecoherencecheck.c` passes Linux and Brook. The guest test checks separate opens,
parallel first-touch mappings, ordinary writes becoming visible to existing
read-only shared aliases, private CoW isolation, reads into a file-backed private
buffer, writes from mapped source buffers (including a previously untouched
page), unmapped shrink/regrow cache invalidation, and aliases after unlink/close.
Brook populates the untouched source during syscall validation; this does not
prove a fault inside the later copy. The guest barrier encourages parallelism
but cannot prove simultaneous kernel misses. The hosted actual-module test
establishes one backend load for eight contending opens and exercises partial
reads followed by success, premature zero/error followed by retry, partial write
and resize errors, zero EOF tails and final lifetime release.

Kernel `1860fb1984d7a5b841a7ccc59c2a8ecdceb043bf02dbdb94880fc75174917282`
passes seventeen hosted tests and both kernel/standalone-test builds. On an
isolated eight-vCPU/4-GiB VM it also passes 44 mapping race cases, 64 delayed
renderer cleanup cases, five I/O gates, thirteen compatibility gates and graphics/
wait lifecycle tests. Heap integrity, source manifests, checked shutdown and all
three offline filesystem checks pass. Evidence: `build/workloads/file-object-20261016/audit.json`
and the strengthened probe run in `build/workloads/file-object-cold-20261016/audit.json`.
Both runs use the identical kernel; the latter changes only the probe and a header
comment. The two full bootstrap paths and browser acceptance are not renewed.

## Verified registration/publication stage (2026-10-16)

The implementation uses one embedded registration per authoritative range node,
indexed weakly by its `FileObject`. The range's existing backing reference retains
inode lifetime; the index adds no strong AS reference or vnode cycle. This is a
per-range alternative to the earlier proposed per-(AS,file) token. Enumeration
uses increasing AS identities to deduplicate split ranges, and acquires an AS
reference under the registry IRQ lock only while its count is nonzero.

Structural transactions snapshot distinct files under AS mutation, retain backing
descriptors, then acquire file gates in stable ID order before reacquiring AS
mutation and validating its generation. Overflow storage is allocated outside AS
mutation; allocation failure leaves metadata unchanged. Incoming MAP_FIXED files
and displaced mappings participate. Fork registers unpublished destinations before
copying leaves. Fault snapshots also validate the file generation. The device-only
removal path filters its exact backing to preserve allocation-free cleanup.

Retired range registrations remain indexed through the full TLB acknowledgement.
Completion unlinks them without acquiring the file gate, allowing a future
truncator to wait for retiring tokens while holding that gate. Ref-zero spaces
also count as retiring until teardown completes; enumeration never resurrects
these spaces. Final backing releases follow acknowledgement and unlinking.

Seventeen hosted tests pass, including actual-module cases for split ranges,
fork and partial failed fork, ref-zero enumeration, delayed acknowledgement,
unlink completion while the file gate is held, overflow allocation failure and
Linux raw-brk return behavior on that failure. Kernel
`2ef76a6110cf168f45aa30a8e074fc29158abe4058ec7274e19adcbe5a7ac9fd`
(module ABI 14, test hooks enabled) passes guest coherence, 44 mapping race cases,
64 renderer frame-entry cleanup cases, five I/O gates, thirteen compatibility
gates, graphics/wait lifecycle checks and heap integrity. Checked shutdown and
all three offline filesystem checks pass. The 380-file source manifest matched.
Evidence: `build/workloads/file-registry-20261016/audit.json`.
The production build has test hooks disabled and matching ABI-14 drivers.

## Mapped resize and writeback still required

The registration stage does not enable mapped truncate or writable regular-file
MAP_SHARED. Existing mapped-backing EBUSY protection remains in place. Add a
target-file invalidation path that accepts an already-held file gate; using the
ordinary transaction lock here would recursively acquire that gate. Before
enumeration, wait for retiring registrations, including ref-zero AS teardown,
to drain without obstructing their acknowledgement and unlink completion.

Truncate holds the file gate to exclude new registrations/publications, acquires
registered AS references safely and preallocates invalidation work. Invalidate
populated and hidden leaves, including private CoW pages beyond EOF, but retain
range metadata. Complete TLB ACKs before releasing frames/recycling blocks.
Coordinate the partial EOF tail and published size, and consult authoritative
backend size after a failed mutation. Add typed EOF/SIGBUS fault outcomes.

Deterministic gates must pause fault/fork/mremap publication against truncate,
last-unmap against enumeration, and AS ref-zero against enumeration; also test
partial EOF, existing private/shared aliases and shrink/regrow. The registration
list alone is insufficient without gates on every publication path.


### Unmapped resize error-path correction

Review of `Ext2FileTruncate` shows that a negative return can follow tail,
indirect-block or inode mutation, including a size change followed by a block-free
failure. The source now evicts the unmapped canonical cache and advances file
generation after every backend resize attempt, preserving its original errno.
The host regression models mutation followed by EIO and checks reloaded bytes,
new EOF and zero tails; it is included in the currently passing 30-suite hosted
run. This is unmapped-resize evidence. Future mapped resize must additionally re-stat
after errors and handle stat failure without republishing stale aliases.


### Mapped-truncate implementation order after registry review

Astra's source review recommends a dedicated `FileTruncateJob`, retaining the
vnode, target file and every affected AS under cancellation protection. It must
not call the ordinary transaction lock while holding the file gate. A target-only
lock acquires AS mutation and asserts the already-held target gate; it does not
acquire gates for unrelated files in the same address space.

1. Hold the file gate and drain retiring registrations, including ref-zero AS
   cleanup, without AS, registry or page-table locks. Completion must be able to
   unlink independently of this gate.
2. Enumerate retained AS references and preallocate all work and leaf-retirement
   storage before destructive changes. Count populated leaves with the sparse
   walker, rather than allocating per virtual page. OOM leaves the backend and
   mappings unchanged.
3. Match authoritative ranges by `backing->file`, translate file offsets, and
   detach affected populated and hidden leaves under one AS lock at a time.
   Preserve range metadata and registrations. Split AS unlock, TLB completion
   and retired-reference release into explicit stages.
4. Complete every required acknowledgement before backend resize can recycle
   file blocks. No AS mutex may span filesystem I/O. Do not abandon a partially
   invalidated job on cancellation.
5. Preserve the valid modified prefix of private CoW boundary pages. Retain
   boundary state through acknowledgement and backend completion; reconcile
   tails against resulting EOF without discarding private data below EOF.
   Whole pages beyond EOF must produce the typed EOF/SIGBUS outcome. Keeping
   ranges allows later growth to fault pages back in.
6. Re-stat after a backend error as well as success, invalidate stale canonical
   pages, advance file generation and preserve the original errno. If stat
   fails too, block affected faults with an explicit error state until a safe
   retry rather than expose old cached mappings. Keep private frames hidden and
   prevent mprotect from revealing them. Block size-changing writes/appends and
   further resize attempts until recovery first observes and reconciles current
   EOF: otherwise shrink followed by regrowth could erase the evidence needed
   to sanitize a retained private tail. Fork/remap must preserve hidden state.

Verification must include delayed fault/fork/remap publication, last-unmap ACK,
ref-zero teardown, hidden mappings, private dirty boundary prefixes, shrink and
regrow, OOM before detach, backend mutation followed by EIO, and failure of the
post-mutation stat. This is an implementation plan grounded in source review;
these mapped-truncate paths and tests are not yet implemented.


### Narrow invalidation primitive verified on the host

`FileMappingInvalidation` now implements the target-gate/AS-lock portion as a
separate type, without general range mutation or publication methods. It counts
and detaches sparse leaves by page-aligned file-offset intersection, keeps all
range nodes and registrations, preflights retirement capacity, and retains
managed frame references through the full TLB acknowledgement. Its caller holds
the target file gate through destruction; it never acquires unrelated file gates.

Actual-module hosted cases cover split, hidden and borrowed aliases, unrelated
files and out-of-interval pages, allocation/capacity failure before mutation, and
a delayed acknowledgement while an unrelated file gate remains held. The full
seventeen-test hosted suite and production kernel/standalone-test builds pass
(`build/workloads/file-invalidation-build.log`). This primitive has no production
truncate consumer yet. It does not implement boundary-page reconciliation,
backend-error size recovery, typed EOF faults or dirty writeback, and neither
mapped truncate nor writable regular-file MAP_SHARED is enabled by this pass.

### Private restoration after acknowledgement (17 October)

The invalidation primitive now separates `Acknowledge()` from reference release.
ACK unlocks AS mutation, invalidates stale translations once, and retains all
detached frames until restoration or destruction. No further detach is allowed
after ACK. The caller must acknowledge every participating AS before restoring
any private data, while retaining the target inode gate throughout.

`PreparePrivateRestore()` reserves one replacement frame per populated
managed-private range before detachment, outside AS mutation. It uses sparse
leaf counting rather than allocating per virtual page. Any observed EOF can
intersect at most one boundary leaf per range, so the limit need not be known
until after the backend attempt and re-stat. Capture identifies managed private frames
by comparing their exact file offset and physical address against the inode's
canonical cache. This is a pure gate-owned lookup, safe inside the PT spinlock.
Canonical, shared and borrowed leaves are not restored by this operation.

`RestorePrivate(validPrefix)` validates all destinations and required copies
before publication, retains full private pages below the observed limit, and copies partial-page prefixes into
separate frames with zeroed tails. It never changes the original private frame:
that frame may still have fork/CoW or independent owners. Original hidden state,
permissions, CoW, PTE tag and PID are preserved. Restored references transfer back
to PTEs exactly once; un-restored originals and unused copies remain owned by the
job until destruction. New frames use the destination AS's PMM allocation PID.

`RestoreAll()` supplies exact rollback after a post-ACK flush failure **before
the backend resize attempt**. It restores canonical, shared, borrowed and private
aliases with original PTEs and bytes, needs no boundary copies, and restores
borrowed map accounting without taking frame ownership. It is mutually exclusive
with private-prefix restoration. An errored backend resize may already mutate
the file and must not use this rollback path.

The actual-module hosted test covers two address spaces, multiple aliases,
hidden pages, an independent retained source owner, preservation of private bytes,
canonical/shared/borrowed exclusion, above-EOF retirement, zero/aligned/full
retention limits, a deferred EOF in each of three different pages, exact rollback
with and without unused copy reservations, heap and partial-frame-allocation failures, delayed ACK with
retained refs, repeated ACK, exact PTE metadata and allocation-free restoration.
All 30 hosted suites pass. The production kernel and module import checks pass;
this path has no production caller, so these are hosted primitive results, not
guest mapped-truncate acceptance. Current logs: `build/private-recovery-host-20261017.log`
and `build/private-recovery-build-20261017.log`.

The supplied limit must be min(old valid private prefix, actual resulting EOF).
The primitive now supports exact pre-attempt rollback and arbitrary observed
EOF. The quarantine stage below handles unknown EOF while retaining private
data. A complete resize consumer must still connect those branches to filesystem
results; a planned prefix cannot substitute for an observed result after a
backend failure. Post-ACK dirty flush, backend resize/re-stat,
cache reconciliation, raw kernel-copy fault recovery and runtime acceptance are
still required. Mapped truncate remains EBUSY and writable regular-file MAP_SHARED
remains disabled.


### Unknown-EOF quarantine and recovery primitives (17 October)

The subsequent [recoverable-copy checkpoint](user-copy.md) adds exact-RIP
assembly fixups and progress-aware regular-file staging. It is a prerequisite,
not completion of the kernel-wide raw-access audit or the resize coordinator.
The hosted suite now contains 31 tests; the 30-suite results below describe the
earlier quarantine checkpoint.

`FileObjectQuarantineLocked` records a negative mapping error, clamps the saved
valid private prefix without widening it, and advances the file generation.
Its caller must already hold the inode gate, have acknowledged every detached
alias, and have successfully flushed canonical pages **before** the backend
resize attempt. Marking quarantine after a failed pre-attempt flush would violate
the cache-retirement contract.

`QuarantinePrivate()` returns noncanonical managed-private frames to hidden PTEs
without changing their contents or allocating copies. Canonical/shared/borrowed
leaves remain detached. The inode error blocks normal reads/writes/appends,
canonical page getters, writable exposure and writeback. Faults produce a typed
I/O outcome; stale prepared faults fail epoch validation. Both mapping creation
phases, accessible publication and nonzero mprotect reject quarantine. PROT_NONE,
unmap, exit, hidden-only fork and remap remain available.

Recovery must first observe current EOF before any further size-changing write,
then capture and ACK all current registrations, including aliases created by
fork/remap during quarantine. `RestorePrivate` clamps its input to the saved
prefix. It restores temporary hidden pages according to current range protection,
so explicit PROT_NONE remains hidden, preserves CoW, and uses separate zero-tailed
boundary copies. The inode error stays set throughout this phase; the specialized
invalidation restorer is the narrow publication exception.

Only after every alias is reconciled may `FileObjectCompleteMappingRecoveryLocked`
evict stale canonical cache, clear quarantine and advance the epoch again. The
global writeback pass also handles a queue-owned orphan: it reports the stored
error, but once registrations reach zero it discards the already-flushed stale
cache and releases the queue vnode outside locks. Independent pins keep their
own frame references and a still-live inode retains its error. It does not claim
that flushing the quarantined file succeeded.

Actual-module hosted tests model backend mutation followed by EIO and a failed
stat, retained private bytes, stale/fresh fault rejection, accessible-publication
and protection guards, blocked file I/O with unchanged offsets/backend bytes,
finite orphan queue retirement, hidden fork/remap, registry enumeration of both
address spaces, recovery clamping after growth, real PROT_NONE versus temporary
hiding, CoW and zero tails. All 30 suites and production kernel/module checks pass:
`build/quarantine-host-20261017.log`, `build/quarantine-build-20261017.log`.

These are connected primitive tests, not guest mapped-truncate error injection.
The filesystem resize consumer and raw kernel-copy fault recovery remain open;
the production mapped-truncate and writable-sharing guards remain in place.

An isolated 8-core/4-GiB production VM also passes the unchanged native file-fault
probe 10/10 as UID 1000 with mapping test hooks off. Its checked sync/shutdown,
QEMU exit and offline data/Nix/FAT checks all finish successfully (exit 0).
Evidence: `build/workloads/quarantine-20261017/audit.json`, session
`2fdfb237151b4d5c9e0e1eff387359c9`, kernel SHA-256
`5ef1f6553816acd302eaec7999d615e92641b3ca19c4954d047347435fe91dd7`.
This verifies normal file-fault behavior with the new kernel/modules; it does
not exercise production quarantine, mapped resize, SQLite WAL or Firefox audio.

### Resident writeback primitive verified on the host (16 October)

FileObject now has a gate-owned writable-exposure hook that checks the exact
canonical frame, and a range-aware writeback primitive. Exposure stays marked
after both success and failure: an alias can store during the snapshot or after
a completed flush. The primitive uses a bounded 4-KiB snapshot rather than
passing live mapped memory to the filesystem, retries short writes, rejects
zero/invalid progress, propagates backend errors, and clips writes to current
EOF. It flushes complete valid prefixes of intersecting exposed pages, never
the mapped tail past EOF. The file gate serializes snapshots against ordinary
write-through operations. No AS or page-table lock spans filesystem I/O.

Actual-module hosted tests cover clean/noncanonical exclusion, selected ranges,
empty/invalid ranges, allocation/stat/write errors, retry after partial progress,
zero progress, unchanged cache ownership, partial EOF, a store injected after
snapshot creation, and an ordinary write blocked behind a paused flush. That
write's final value survives in both the canonical page and backend. All 19
hosted tests pass, as do the production kernel/driver and standalone-test builds.
Evidence: ignored build/workloads/shared-writeback-host-final.log and
build/workloads/shared-writeback-kernel.log. The initial host configuration used
the unwrapped compiler and failed to link; the verified run uses HOST_CXX.

This is an inactive foundation: no production caller marks pages writable or
invokes mapped writeback yet, and writable regular-file MAP_SHARED remains
rejected. Still required before enabling it: dirty backing retention after final
unmap/close, background and global sync integration, range-aware msync, mapped
resize including private boundary prefixes and backend-error recovery, typed
EOF faults, and the complete guest/race/reboot acceptance above. A host snapshot
test does not establish real writable aliases or persistence across reboot.


### Writeback retention and VFS sync integration (16 October)

The exposure hook now retains one vnode per queued file object. A global pass
takes temporary vnode references under a short queue spinlock, then performs I/O
under each file gate without the queue lock. It visits a finite identity-bounded
set, continues after individual errors, and returns the first error. Successful
flushes release the queue's reference only when no mapping registrations remain.
The registry maintains an atomic count including retiring/ref-zero registrations
until their actual unlink after acknowledgement; its list/count consistency is
checked. Read-only registrations conservatively retain the same pending object.
Final vnode release runs outside all file/queue locks. Failed I/O retains pages
and backing rather than silently discarding stores.

VFS fsync/fdatasync now flush exposed pages before backend durability; global
sync attempts queued writeback and every mount/device flush, preserving the first
error. Clean objects avoid snapshot allocation and filesystem writeback. Unmapped
truncate flushes pending exposure before cache eviction and leaves the resize
unattempted if that writeback fails. These changes do not enable writable mappings.

The actual-module host fixture verifies repeated flush/redirty while a token
remains, one retained vnode across repeated exposure, continued flushing of a
second object after the first fails, backing survival after user close, failed
resize preflush, allocation failure/retry, and final-reference disposal only
after successful flush with zero tokens. The fixture models its token; the real
registry and delayed-ACK tests run separately in test_vmm_leaf. All 19 hosted
tests and production kernel/driver/standalone-test builds pass.

Remaining integration includes a periodic worker, msync and every writable
publication/protection path, mapped truncate/error recovery and typed EOF faults.
A successful host flush does not yet establish guest mapped-write durability.

Before admitting shared writable aliases, also test ordinary writes that extend
EOF after a mapped store into the old partial-page tail. Canonical gap/tail
zeroing must prevent such bytes becoming newly valid file content on growth.


Kernel bc125307791a3cb7961ba9ed6dabfae318f450cd84be3473d3fb5382a1948137
passes an isolated eight-vCPU/4-GiB regression: ordinary file coherence, inode
identity, the existing unmapped-truncate/mapped-EBUSY contract, one/four receiver
Unix rights (512/512 each), and Firefox local rendering, one click, exact input
and normal exit. The first identity attempt omitted its directory; after fixture
setup was corrected, the entire strict sequence completed with exit 0. A second
boot verified the ordinary truncate fixture. Both boots returned to ten baseline
processes, passed heap/page checks, checked shutdown and all three offline fscks.
Evidence: ignored build/workloads/shared-writeback-20261016/acceptance.json.
These guest tests do not exercise queued dirty mapped pages: writable shared
regular-file mappings are still rejected, and the exposure hook has no runtime
caller. Both full bootstrap paths remain to be renewed.
