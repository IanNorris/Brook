# Coherent regular-file mappings

Implementation target following the mapping-transaction checkpoint of
2026-10-16. Writable regular-file MAP_SHARED remains disabled until the connected
path below passes. The work must cover ordinary I/O as well as mmap.

## Current gaps

Ext2 now embeds a `FileObject` in its existing retained live-inode holder.
Separate opens share that object; mappings retain a vnode and therefore its
inode holder. Quiescent last-holder/orphan disposal drops cached frames without
a sleeping lock or filesystem I/O. There is no permanent vnode/object cycle,
and a reused inode gets a fresh object. `VnodeOps::file_object` exposes the
borrowed object while a caller retains its vnode; modules use ABI 13.

File faults now prepare canonical pages through that object outside AS mutation.
One per-inode gate serializes loading, so concurrent faults retain one completed
frame. Required bytes are read to completion across short progress; premature
zero/error is rejected without publication. Only the actual EOF tail is zeroed.
The fault profiler distinguishes new loads from hits.

VfsRead and VfsWrite now use the same gate and cached frames for ext2. Reads
currently fetch backend bytes and overlay resident pages. Successful ordinary
writes update only their written prefix in resident pages; errors preserve the
cached data. User buffers use bounded 64-KiB staging with copyin/copyout outside
file and filesystem locks. Unmapped truncate invalidates cached pages after a
successful backend resize. These are write-through operations; mapped stores,
dirty writeback and msync are not implemented.

The older clean cache in `memory/file_page_cache.{h,cpp}` remains the fallback
for other filesystems. Both ext2 regular files and FAT still set the old cacheId
to zero; ext2 now uses its live-inode object instead. FAT has no file-object
accessor and does not acquire ext2's new coherence guarantees.
VfsTruncate still returns EBUSY while any file mapping pin exists. There is no
msync implementation. The existing boolean fault result cannot distinguish
EOF/SIGBUS from an invalid address/SIGSEGV.

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

The live-inode object and canonical loading are implemented for ext2. Next add
mapping registrations, size/publication coordination and dirty writeback to that
object. Its inode-holder references provide the lifetime shared across opens. FAT's current directory-entry identity is valid
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


### Resize error-path correction awaiting verification

Review of `Ext2FileTruncate` shows that a negative return can follow tail,
indirect-block or inode mutation, including a size change followed by a block-free
failure. The source now evicts the unmapped canonical cache and advances file
generation after every backend resize attempt, preserving its original errno.
The host regression models mutation followed by EIO and checks reloaded bytes,
new EOF and zero tails. These edits are not yet built or run: the user is testing
the frozen production checkpoint. Future mapped resize must additionally re-stat
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
   retry rather than expose old cached mappings.

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
