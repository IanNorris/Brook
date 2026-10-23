# Steam shared-memory checkpoint — 23 October 2026

This checkpoint gives `/dev/shm` a real tmpfs mount, enables quota-accounted
tmpfs file mappings, and makes shared FileObject futexes match across processes.
It removes the observed controller IPC and shared-memory initialization errors.
Steam subsequently exits with code 139; a usable login window or authentication
is not accepted by this checkpoint.

## Two separate boundaries

An isolated tmpfs fixture first reproduced `mmap(MAP_SHARED)` returning
EOPNOTSUPP. The tmpfs backend deliberately disabled mappings. After enabling
and testing those mappings, a small static probe run through the real SteamRT
wrapper still failed at file creation, before mmap. Its `/dev/shm` reported FAT
filesystem type `0x4d44`: it inherited the historical directory placeholder,
without an actual shared-memory filesystem. Mounting tmpfs at the canonical
path changed the same probe to type `0x1021994` and successful create, truncate,
shared-map and store operations.
The source is retained as `tools/at-syscall-probe/shmfilecheck.c`.

The kernel now provisions a fresh initial-namespace tmpfs at `/dev/shm` before
userspace starts. It has root ownership, mode 01777, NODEV and NOSUID. The initrd
already supplies the mount-point directory. Namespace copies and recursive
binds retain the actual filesystem. Existing retained-path broker rules suppress
the legacy global named-shm shortcut beneath the fresh mount; ordinary VFS
permissions, unlink lifetime and filesystem identity apply. The canonical
`/dev/null` and urandom anchors remain separate. Provisioning failure is a boot
error; this does not add mutable-FAT directory creation support.

## Mapping lifetime, quota and IPC

Tmpfs reuses the existing canonical FileObject cache and mapping registrations.
Shared aliases see the same pages; ordinary reads/writes and explicit/periodic
writeback remain coherent. Private mappings preserve COW. Mapped truncate stays
EBUSY, consistent with the existing VFS contract. This does not implement mapped
resize, general tmpfs execution acceptance or arbitrary mount options.

Optional immutable FileObject callbacks charge canonical frame and metadata
storage against the owning tmpfs quota before allocation. Allocation failures
unwind that charge, and every canonical-cache retirement path releases it.
Callbacks use a retained inode context; final quota release is non-sleeping.
Module ABI is 48, requiring a matched kernel and all modules.

Sparse backing is materialized before publishing a canonical page, so later
writable exposure cannot exhaust the capacity needed for writeback. A later
canonical allocation failure may leave a charged zero backend page; it does not
extend EOF, and inode teardown reclaims it. Backend and canonical storage are
both charged honestly. Defaults remain 16 MiB per instance and 256 MiB globally;
this is not whole-process/private-COW memory accounting.

This preparation avoids a concrete retention cycle: otherwise canonical pages
could consume all quota, shared stores could require another backend page on
writeback, and ENOSPC could retain an unlinked inode through the writeback queue.
The hosted and native tests explicitly exercise full-quota writeback followed
by unlink, close, unmap, sync and reclamation.

Non-private shared FileObject mappings use a separate tagged object-ID/offset
futex domain. Both FileObject and memfd identities stop before the tag boundary
and are never reused. Private keys remain per thread group and address. Mapping
admission still spans key derivation, resident-leaf/value checks and enqueue;
no object pointer is retained by the waiter. This applies to canonical
FileObject mappings, tested on tmpfs and Ext2, rather than every filesystem.
Shared WAKE_OP/PI and other previously unsupported operations remain rejected.

## Validation

All 101 rebuilt host tests pass. Four affected fixtures pass ASan/UBSan: tmpfs,
FileObject, mapping pins and mapping ranges. Astra reviewed quota/writeback
progress, callback lifetime, disjoint futex keys and boot mount ownership.
Entry-state and scheduler mutation audits pass on the final ELF.

Native acceptance includes shared/private coherence, independent opens,
SCM_RIGHTS aliases at distinct addresses, offset/object separation, interruption,
cancellation, remap stress, unlink/recreate separation, mapped-resize rejection,
read-only bind restrictions and full-quota sparse writeback/reclamation. The
19-case alias suite passes separately on tmpfs and Ext2. A fresh boot verifies
canonical tmpfs type, mode 01777 and NODEV/NOSUID, then runs the same alias suite
as the ordinary desktop user without a manual mount intervention.

The portable static tmpfs fixture passes its baseline mode (52 fixture checks,
five parent checks) and Brook mode (358 fixture checks, 24 parent checks).
Its reclamation check now calls sync after close, because writable mappings
introduce the existing deferred writeback queue. The earlier immediate-close
assertion failure remains recorded. Unsupported-unshare/normal-unmount attempts,
a missing `gcc` profile alias and oversized bridge requests also remain recorded;
they are not counted as native acceptance. Packed source-owned static probes
ran within the bridge's 32 KiB request limit, without exporting binary payloads
into chat or changing live boot/kernel images.

With a real canonical mount, fixed diagnostic counts for controller m_rgPipe,
SteamController_Master_mem, /dev/shm, fatal assertions and unsupported-operation
messages fell to zero. The later client exit is 139. Full raw Steam logs stay
inside the guest; the launcher now reports paths, sizes and hashes by default,
with raw bounded tails an explicit opt-in. Tests use the previously staged
client/interpreter patches and skip bootstrap. No updater, login or QR-code
acceptance is claimed.

Ignored evidence: `build/workloads/steam-shm-20261023/` and its baseline and
candidate reviews. The final boot ELF SHA-256 is
`6e5b7c181916fa6cacf102af1a4d010546c662e6c9427e0bc34d8adf90599c6c`.
All dispatched jobs drained before the final heap check and clean shutdown;
boot, data, Nix and temporary disks each passed offline fsck with exit 0.
The close controller initially saw a cached queued server status; it recovered
the original Xwayland job before issuing its first close request. That failure
is preserved separately from the successful shutdown.
The next bounded task is to locate the client exit after controller initialization,
keeping the real shared-memory and GLX checks as regressions.

The following [robust-futex checkpoint](steam-robust-futex-checkpoint.md)
identifies and removes that exit-139 assertion, with a separate later startup
failure still under investigation.
