# Bubblewrap compatibility checkpoint

The accepted user-namespace foundation is `d7600678`. This follow-up tests the
shipped bubblewrap unchanged, fixes its first reporting prerequisite, and records
the remaining implementation boundaries. Bubblewrap has not launched its child
command on Brook; Steam login remains unavailable.

The subsequent accepted [proc namespace-handle checkpoint](proc-namespace-handles.md)
resolves the proc access failure recorded below. The identical executable now
stops at unsupported mount propagation (`Failed to make / slave: Invalid argument`).
The observations here retain the earlier checkpoint history.

## Observed progress

The private Steam payload's `srt-bwrap --version` reports bubblewrap 0.12.0.
The diagnostic runs as UID 1000 with `--unshare-user --bind / / -- /boot/BIN/NNP`.
This deliberately broad bind is an integration smoke test, not confinement or a
Steam launch. The executable and arguments remain unchanged between comparisons.
Its existing staged interpreter/library environment is reused; no namespace or
security checks in the program are patched out.

The accepted kernel first returns:

```
bwrap: Can't read /proc/sys/kernel/overflowuid: No such file or directory
```

Brook already uses `UserIdOverflow` (65534) for unmapped ordinary credential IDs.
Procfs now exposes that same fixed policy through `overflowuid` and `overflowgid`,
with retained `sys` and `sys/kernel` directories. Enumeration, directory-relative
lookup and parent traversal use the retained object hierarchy. This reporting is
read-only; writable Linux sysctl configuration is not implemented or advertised.

The corrected kernel passes native enumeration, openat through a directory FD,
short reads, EOF, seek/re-read and write-open denial. The identical bubblewrap
binary advances to a different failure, reproduced verbatim:

```
bwrap: open /proc/22/ns/ns failed: No such file or directory
```

Its parent exits 1 while a child waits for setup. This makes a pipe-reading
`subprocess.run` wrapper wait despite the parent's exit. The observer timeout is
preserved. Task identities and the blocked child are captured before explicitly
stopping that child; recovery then collects the original parent's error. That
cleanup is not a successful bubblewrap launch. The error does not establish the
complete namespace-handle contract; source and differential inspection must
resolve the requested path and descriptor semantics before implementation.

## Architectural boundaries

The retained Linux SteamRT trace includes 314 mount calls in the main setup
child, including 244 ordinary recursive binds, one initial recursive self-bind,
59 read-only bind remounts and two root switches. It also mounts fresh tmpfs
instances, sets private/slave topology and applies noexec. The two root-switch
forms are `pivot_root("/tmp", "oldroot")` and `pivot_root(".", ".")`, each followed
by detach operations. These are requirements observed before authentication,
not optional flags inferred from a generic container feature list.

Astra's source review identifies the following implementation gates:

1. Proc namespace descriptors must represent actual retained namespace identity
   and lifetime. PID reuse and task exit must not redirect an existing handle.
   Unsupported setns/unshare operations must continue to fail explicitly.
2. File/socket binds need compatible source/target types and retained source
   objects/backings. UNIX socket aliases must resolve the same inode identity.
   Non-directory roots cannot use legacy directory-path projection.
3. Recursive binds need a transactional clone of the reachable attachment
   subtree, including stacked mounts and inherited restrictions. Allocate and
   validate before publishing; rollback must leave no partial topology. Textual
   prefix matching cannot establish subtree ownership.
4. Attachment attributes require enforcement at existing writes, append,
   truncate, metadata mutations, shared writable mappings, execution and
   executable mapping/protection changes. A read-only label on a writable
   attachment is insufficient. Define writer/mapping admission and remount
   synchronization with Linux differential cases before adding success paths.
5. Fresh tmpfs requires independently owned filesystem instances with mutable
   retained objects and allocation/teardown behavior. Binding the existing `/tmp`
   disk does not supply those semantics.
6. Pivot-root requires topology relocation and coherent affected filesystem
   contexts. `FsContext` currently changes cwd but has no root-update transaction.
   Define which root/cwd references move, how shared contexts participate, and
   how snapshots synchronize with publication. Preserve legitimate held outside
   descriptors. Recheck proc/dev and legacy textual brokers against the new root.

Private/slave requests may be implemented narrowly for topology proven to have
no propagation relationships. Do not acknowledge unsupported shared/master
relationships as private. Mount restrictions belong to attachments, not shared
filesystem backings, so sibling aliases can have different enforced policies.

These are filesystem, mapping and process-publication changes. The unchanged
bubblewrap target is larger than one mount-dispatch extension. The bounded phase
must stop before introducing those semantics without their respective designs,
Linux comparisons and native acceptance; the proc reporting fix is independently
reviewable. No new mount or root-switch success path is introduced here.

## Validation and evidence

Evidence is private under `build/workloads/bubblewrap-20261022`. The original
baseline review is `build/reviews/bubblewrap-off-gpu-1ms-fixed-mlfq-a-20261022`;
the corrected review is the corresponding `mlfq-b` directory. The shipped
executable SHA256 is
`91f2b55f25ecff638f0357abb9cc18fa1f010fd9e48e8fb59b96196827052a6c`.

The corrected ABI-41 kernel SHA256 is
`88d301233910849a8dd79f917709f59dd1109f0397fb4672aca761c9440c1c29`.
Its 13 modules and source diff are frozen in `kernel-proc-overflow`.
All 99 existing host tests pass, and compiled entry/start structural audits with
mutation checks pass on that exact frozen kernel. Native user-namespace acceptance,
all nine filesystem/credential/IPC regressions and normal Mousepad launch, capture
and graceful registered-client-group exit pass. Final heap integrity is OK.
All tracked jobs end with zero live tasks after the explicitly recorded blocked
child cleanup. Checked shutdown completes, the original launcher exits zero and
all four offline filesystem checks exit zero. The baseline comparison also shuts
down cleanly with four successful disk checks. Kernel builds/staging occur only
with all QEMU instances stopped.

Astra's final review finds no blocking proc ownership/traversal issue and recommends
ending at the larger design boundary. This closes the bounded diagnostic outcome;
it does not complete unchanged-bubblewrap trivial-child acceptance. The next
implementation slice should specify and test the observed namespace-handle access
before advancing the mount sequence. The fixed sysctl values intentionally do not
provide writable Linux sysctl administration.

The source review uses [bubblewrap 0.12.0](https://github.com/containers/bubblewrap/blob/v0.12.0/bubblewrap.c)
and [Linux mount implementation](https://github.com/torvalds/linux/blob/v6.12/fs/namespace.c).
The retained trace provides the exact shipped runtime's observed requests;
upstream source alone is not proof that Brook supports them.
