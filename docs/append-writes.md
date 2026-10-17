# Append writes and package registration

The descriptor write path previously ignored O_APPEND for ordinary vnodes.
Independent opens wrote at their descriptor offsets, overwriting earlier data.
An affected Nix manifest retained Codium's row plus the tail `78.96` from the
previous Chromium row. A prior Firefox preference append also overwrote its
prefix. Neither failure should be worked around by assuming append-at-open is
sufficient: each write must select the current EOF.

## Implementation

For ext2, EOF selection, backend write, resident mapped-page overlay and cache
generation publication occur under the shared live-inode FileObject gate.
Truncate uses the same gate. FAT selects fresh EOF under its backend mutex after
synchronizing alias metadata, then writes and publishes the changed metadata.
There is no stat-then-write fallback for other backends: unsupported append
returns EOPNOTSUPP instead of racing independent opens.

The syscall copies at most 64 KiB before acquiring these locks. Larger writes
may return a short result; callers retry the remaining bytes as another append
transaction. writev gathers one bounded prefix across vectors, so each accepted
prefix is contiguous even with competing writers. pwrite honors Linux's
O_APPEND behavior while preserving the descriptor's current offset. Readonly
vnode descriptors reject write/writev/pwrite with EBADF. O_SYNC/O_DSYNC retain
their existing flush policy. Ordinary vnode writes are bounded to 0x7ffff000
bytes, avoiding request narrowing or overflow of the backend's signed result.
The installer now also checks buffered output and
close errors before reporting that the manifest write succeeded.

This does not implement shared open-file-description offsets/status flags across
dup/fork. Independent-open append atomicity does not establish those semantics.
FAT retained-open rename/unlink and directory lifetime restrictions also remain.

## Verification

Run `python3 scripts/workloads/append.py RESULT.json FIXTURE_PARENT` on Linux,
or launch `/data/append.py` with the same arguments inside Brook. The flat,
private fixture checks stale offsets across independent opens, F_SETFL, readonly
descriptors, positioned append, zero writes, libc append streams, four concurrent
writers, vectored records, retained renamed/unlinked ext2 files, and a 262147-byte
vectored payload with short-write retries. Cleanup failures are reported rather
than hiding the test results.

The old kernel (`9b3efe47b5b55d2d64761a73785be995b6df039b0b02adbe11c3880761204288`)
failed six of seven groups in session `c687813134fd4f559cd502eaa0494a15`.
The glibc stream example passed even there, so it alone does not reproduce the
musl installer's corruption. The fixed kernel
(`a9f0b96ae90436176bdadb4d3c27f5a78a8162ecd47c465b37a17650d05bc57f`)
passed all seven groups on both /data and /nix in session
`86e9d7c35a26424ca58e9bc4f3b8e1d5`. FAT passed six groups and rejected retained-open
rename/unlink with EBUSY; its directory cleanup also returned EBUSY. These are
recorded limitations, not passing POSIX acceptance. Linux passed all seven groups.

All 21 hosted suites passed, including actual FileObject tests of eight
concurrent independent writers, resident cache coherence, short/error/zero
writes, and append-versus-truncate serialization. The kernel built twice to
refresh generated symbols; all twelve matching driver modules were staged.

The actual musl installer installed `firefox-stock` then `chromium-stock` from
existing closures in an isolated clone. Before repair, its corrupt manifest was
saved as `/data/manifest-before-append.tsv`; only the known malformed tail was
removed. The resulting manifest preserved Codium, Firefox and Chromium rows.
Firefox launched through the refreshed native menu, rendered an HTTPS page,
and exited through its window close control. Native Terminal confirmed
UID 1000, GID 100 and USER/LOGNAME brook with HOME /data/home. The run returned to
ten background tasks with heap integrity OK, then passed checked shutdown and
all three offline filesystem checks. Evidence resides in
`build/workloads/append-20261017/` and the named bridge session.

Reboot session `4f5313ed306445708efc27d3e24b566e` verified byte-for-byte manifest
persistence, the Firefox wrapper in the launcher index, all seven append groups,
fourteen root credential groups and six default-Brook groups. GIMP opened and
quit normally, but its legacy root-owned settings rejected updates; that profile
migration remains a separate acceptance requirement.
The reboot run returned to ten tasks with heap integrity OK; checked shutdown
and all three offline filesystem checks passed again. The tracked installer
binary is rebuilt from the tested source so disk creation includes the fix.

The final ordinary-write bounds correction was built twice and retested as
kernel `b2e39936b95b550a98ed887ffb98899c51cb1a5d500fbd542b115ec6e570f02f`
in session `77beff8ad21e4790878b0bec4765c672`: seven append groups, fourteen root
credential groups, ten remaining tasks and heap integrity OK. Checked shutdown
and data/Nix/boot offline filesystem checks all passed.
