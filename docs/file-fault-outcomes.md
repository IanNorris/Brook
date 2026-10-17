# Typed regular-file fault outcomes — 17 October 2026

Whole-page file EOF now produces user `SIGBUS` with `BUS_ADRERR` and the original
fault address. Ordinary invalid/protected mappings remain `SIGSEGV`. Both
SA_SIGINFO delivery and unhandled fatal-group termination use the typed outcome.
This is a prerequisite for mapped resize, not writable shared-file acceptance.

`FileObjectFaultPages` samples size under the inode gate, bounds neighboring
cached pages by both EOF and the VMA, and prepares the primary canonical page.
It separates EOF, backend I/O failure and allocation failure. A backend `ENXIO`
must not be mistaken for EOF; known stat data is passed into the loader rather
than inferring EOF from an overloaded errno. Partial-page tails remain zero.

The fault resolver routes EOF/I/O results through the same phase-2 test barrier
and AS/file-generation validation as successful publication. For ext2's retained
inode objects, growth changes the file generation; remapping or protection
changes alter the AS generation. The old result is discarded and the current
mapping retried. FAT's legacy cache does not acquire this inode-coherence
contract. Prepared page/backing references
remain transaction-owned through errors and retries.

The exception path maps validated EOF to SIGBUS/BUS_ADRERR (2) and validated
backend I/O errors to SIGBUS/BUS_OBJERR (3). Allocation failures stay distinct;
this does not introduce a complete OOM policy across all legacy resolver paths.
Saved CR2 is used for siginfo, including non-page-aligned accesses, after any
diagnostic work. The final fatal fallback also preserves SIGBUS instead of
recomputing SIGSEGV from the hardware page-fault vector.

## Verification

All 30 hosted suites pass. Actual file-object tests exercise EOF without I/O,
partial tails, bounded cached neighbors, stat/read/allocation errors, ENXIO versus
EOF, ENOMEM classification and reference cleanup. Production/test kernel builds
and all 12 module import checks pass.

`scripts/workloads/file_faults.c` compiles with the existing static musl compiler.
The same frozen probe binary runs on Linux and each guest. Parent and child use
raw alarm watchdogs so a signal or cancellation regression cannot hang the
probe indefinitely.

| Run | Result |
| --- | --- |
| Linux control | 10/10 |
| Unchanged Brook baseline | 3/10; all seven EOF cases incorrectly report SIGSEGV |
| Candidate test kernel | 13/13, including three deterministic stale-EOF races |
| Hooks-disabled production, UID 1000 | 10/10 |

Cases cover empty files, private read/write EOF, shared read-only EOF, nonzero
file offsets, valid partial-page zero tails, protection faults remaining SEGV,
unhandled SIGBUS wait status, mapping lifetime after closing the original fd,
and checked syscall buffers at EOF. The latter forces raw `clock_gettime`:
libc's vDSO implementation writes in user mode and appropriately faults itself,
so it would not test kernel uaccess. IO-error-to-SIGBUS delivery is supported by
the source path; backend I/O classification is host-tested, not injected into
the running guest filesystem.

The test-only root ABI pauses a real fault after EOF preparation. Concurrent
`pwrite` growth must retry and read the newly valid byte; MAP_FIXED replacement
must resolve the new anonymous page; mprotect(PROT_NONE) must produce SEGV, not
stale SIGBUS. Test hooks remain disabled in the production build.

The production probe also passes through the normal maximized Terminal UI,
whose saved native-resolution crop shows `FILE_FAULT_RESULT 10/10`. Chromium
navigates to Example Domain (window title observed), while Codium restores,
edits and saves its document with verified ownership `1000:100`. Both native
closes release their registered groups; the desktop returns to ten tasks and
about 283 MB of physical memory used. These are regression smoke checks, not
complete browser/editor or Firefox audio acceptance.

## Evidence identities

Ignored local evidence lives in `build/workloads/file-faults-*-20261017/`:

- Baseline session `c1a001ea3ed84b3abb30103b5c8134a2`, kernel SHA-256
  `6daaa932d90c6dafd1f8b18c7dd1a15cdca8e3cf8949c2fe304fe176efe68de4`.
- Test-hook session `c45ca28904d84b25a8434b23b53916eb`, kernel SHA-256
  `116821d45b39d53564aab8305023cb5fef656e9b03f420a000a385134e886b4c`.
- Production session `a80dd32d6d634f4b9e290734953773e8`, kernel SHA-256
  `b4e9d7aab5547d52a024ff5ea58df69fd4630a222a419674202123539c4af132`.

Each acceptance directory saves the original request ID, completed stdout/exit,
and shutdown/filesystem checks. Artifact manifests include the exact probe and
kernel hashes. Build/control logs are `build/file-fault-final-build.log`,
`build/file-fault-freeze-hooks.log` and `build/file-fault-final-probe.log`.
All guests use eight CPUs, 4 GiB RAM and software 1080p display. User faults are
intentional in these tests; they must not be misreported as new app regressions.
All three runs acknowledge checked filesystem/device flush, exit QEMU with code
zero, and pass offline data/Nix/FAT checks. The production screenshot is referenced
by `acceptance/terminal-native-crop.json`; the complete frame and clean source
remain in the session artifact directory after shutdown.

## Remaining shared-file work

Writable regular-file MAP_SHARED still returns EOPNOTSUPP, and mapped truncate
still returns EBUSY. SQLite WAL and Firefox profile/audio acceptance remain open.
The [shared-mapping plan](file-shared-mappings.md) remains the implementation
target. The next resize consumer needs explicit detach, TLB acknowledgement,
backend reconciliation and reference-release phases.

Full-file sparse invalidation is a reasonable initial implementation: retain all
affected spaces and private leaf state, then quiesce every alias before flushing
the canonical cache. A preflush alone misses stores before TLB acknowledgement.
Preserve private modified prefixes, hidden mappings and original leaf flags;
re-stat even after backend errors before reconciling EOF and cache generation.

If post-mutation stat fails, private data must survive in inaccessible ownership
until recovery or unmap/exit. A file-owned job retaining AS references creates a
potential AS→file→AS cycle. Hidden private leaves plus an explicit quarantine
state are an alternative, but mprotect must not expose them before recovery.

The checked-uaccess EOF case above does **not** cover validation followed by a
concurrent truncate/unmap before a raw kernel copy. Existing unresolved
kernel-origin faults can still reach panic. Add a deterministic regression and
a scoped uaccess recovery path before accepting mapped resize; never redirect a
kernel fault into user signal delivery while arbitrary kernel frames/locks live.
