# Mapping lookup cache and Firefox diagnostics — 17 October 2026

The address-space mapping list remains authoritative. A 16-entry positive
lookup cache now avoids rescanning it for recently accessed pages. Entries
contain range metadata only: page-table presence, permissions and copy-on-write
checks still run normally. All access uses the existing address-space mutation
lock (or exclusive unpublished construction). Replace, protection changes,
extension and destruction invalidate every entry before modifying nodes.
Fork constructs a fresh table; cache pointers must never be copied between
address spaces.

## Why this change

The preceding Firefox run had 1,658 mapping ranges in its main address space.
A two-second profile contained 18 mapping-lookup samples among 1,183 total
samples. Three coherent paused snapshots found the address-space mutex owner
running in `MappingRangeFind`, with matching lock/task identities. This supports
a costly linear lookup, not a persistent mutex deadlock. A separate one-second
monotonic sleep measured 0.999975 seconds.

The hosted benchmark uses the production lookup and an independent linear
lookup over 1,658 ranges, with 200,000 lookups per workload. One run measured:

| Workload | Linear ns/lookup | Cached ns/lookup |
| --- | ---: | ---: |
| Eight hot pages near the end of the list | 6,180.84 | 2.41 |
| Cycle through all 1,658 ranges | 2,481.28 | 2,504.08 |
| Eight hot pages, replacement every 256 lookups | 6,188.81 | 249.36 |

These are lookup costs, not application speedups. The candidate's two-second
Firefox profile had six lookup samples among 1,101 samples, but more context
switches (33,949 versus 29,209). The browser phases were not controlled, so
these captures do not establish an end-to-end improvement or navigation fix.

## Validation and local evidence

All 29 hosted suites pass. The mapping tests warm the cache before randomized
mutation and cover protection splitting, unmap/remap, extension, destroy/reuse,
table isolation, collisions and boundaries. The guest anonymous-memory suite
passes all eight cases, including sparse pages, kernel copyout, fork/COW,
protection, discard, population and shared mappings.

Chromium and Codium mapped together through the installed-app launcher. Codium
restored its existing document, accepted an appended line and saved it; a guest
read verified the content and owner `1000:100`. Both native closes released their
registered groups. The desktop returned to ten tasks and about 305 MB physical
memory used. Checked shutdown acknowledged filesystem/device flush, QEMU exited
zero, and offline checks of ext2 data, ext2 Nix and FAT boot all exited zero.

Local evidence is intentionally ignored by Git:

- Build, symbol checks, hosted tests and benchmark: `build/mapping-lookup-build.log`.
- Baseline session: `b275e7a884ae49c1b3349733673f4848`, kernel SHA-256
  `20f62b2d0c78010b51c966dea595c15f5007961cacf4c0aef68a0393a0e0ba30`.
- Candidate session: `130c4b1ce88c41dc94bc15a5dafb9aa4`, kernel SHA-256
  `47176e367ace716a8385d1ed9f421b3c8989411d9d1de59a485502d82cfb8991`.
- Baseline captures: `build/workloads/action-workloads-20261017/firefox-audio-current/`.
- Candidate captures and exact operation identities:
  `build/workloads/mapping-lookup-20261017/acceptance/`.
- Baseline/candidate Speedscope artifacts: `f4dfec911aa44f269fca9377bbe28572`
  and `094e356189cf4a8b9cd5c6de6400c11f` in their session artifact directories.

Both guests use eight CPUs, 4 GiB RAM, software Wayland at 1080p, and the silent
QEMU audio backend. The Wayland provider is unchanged. The candidate uses
clones of the checked, stopped baseline disks.

## Firefox findings remain separate

An initial direct launch without the Wayland provider faulted; that is a test
setup failure and is preserved in the baseline serial log. Normal UI launch
starts the provider. Both existing and isolated profiles then took a long time
to navigate. The baseline fixture server eventually recorded one HTML request,
but no video request. The candidate later rendered the fixture and loaded its
video (`readyState=4`), displaying a bookmarks/history database warning.

After a native click on Play, the candidate reported `paused=false` and media
time changed from about 1.02 to 2.51 seconds. The five-second fixture loops, so
these separated observations do not establish its playback rate.
A five-second VM-only WAV capture contained zero PCM frames. This is failed
audio acceptance, not proof of a working audio stream. The candidate's native
Firefox close did release its exact registered client group; earlier baseline
close attempts needed scoped maintenance kills.

A separate ext2 probe on a fresh unlocked inode found `F_GETLK` returning success
while leaving `l_type=F_WRLCK`, instead of `F_UNLCK`. SQLite 3.51.2 passed a fresh
rollback-journal database transaction but failed a fresh WAL transaction with
`SQLITE_PROTOCOL` ("locking protocol"). Both modes pass on the Linux host.
The initial probes accidentally used FAT `/tmp` and failed during temporary
file cleanup; only the corrected `/data/home/.cache` runs support these findings.

The equivalent combined reproducer is now versioned as
`scripts/workloads/sqlite_locking.py --directory /data/home/.cache` (invoke with
the guest Python). Its Linux control passes all three checks. The recorded guest
results above came from the preceding separate probes; the combined script has
not yet been rerun in the guest. It exits nonzero for a failed case and prints
each result before starting the next, potentially slow SQLite protocol attempt.

`sys_fcntl` currently pretends success for unimplemented record-lock commands.
SQLite's Unix VFS uses `F_GETLK` for its WAL shared-memory dead-man-switch check;
an unchanged write-lock query can be mistaken for a competing owner. See
[SQLite's Unix VFS](https://github.com/sqlite/sqlite/blob/master/src/os_unix.c).
This explains a concrete database compatibility failure; it does not yet prove
the cause of every Firefox stall.

Implement actual inode-associated record locks before reporting an unlocked
query. An unconditional `F_UNLCK` response with no exclusion would make concurrent
writers unsafe. Required coverage includes independent opens/hardlinks, reader
sharing/writer exclusion, range conversion and splitting, process ownership and
PID reuse, noninheritance across fork, and release on logical close of **any**
descriptor for the inode (including dup2, CLOEXEC and exit). Deferred FD resource
finalization is too late and may run in another task. Blocking waits need
cancellation-safe teardown; unsupported variants must fail explicitly.
