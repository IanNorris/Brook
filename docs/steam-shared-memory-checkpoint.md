# Steam shared-memory checkpoint — 23 October 2026

## Full-quota unlink regression — 4 November

Steam's later Dell session left little free space in `/dev/shm`, and its web
helper reported shared-memory corruption and stopped responding. This does not
establish that unlink failure caused the Steam failure. A separate regression
was nevertheless reproduced: with a full tmpfs, VFS pathname lookup allocated
an object against the same quota and could return `ENOMEM` before unlink could
release any space. Tmpfs now retains an inode-owned, quota-accounted object
instead of allocating a new one for every lookup.

The hosted test exhausts both the instance and aggregate budgets, checks a
retained file after unlink, and races lookup against unlink and recreation.
The release kernel builds, and the hosted suite passes under GCC and under
ASan/UBSan (leak sanitizer disabled in the restricted host environment; the
fixture separately tracks allocations). Astra reviewed the lifetime design.

The static `tmpfsfullunlink.c` probe ran in a private mount namespace on a
disposable, offline QEMU guest. It varies metadata consumption across 480
64-KiB tmpfs instances and verifies `unlinkat`, link count, and continued reads
from the held descriptor. The earlier Dell candidate kernel failed at trial
155 with `ENOMEM` and link count 1; the candidate passed all 480 trials. These
are QEMU tests; no Dell activation or Steam recovery claim follows from them.
The earlier and candidate kernel SHA-256 values were respectively
`1baa34fedd117d44c941393770d8cb9994455383fad66b7d2a83e3c80c03e89a` and
`c7e83ceebaa1ac20fc17f9152bd71939e14309717ea9ca5a6441bd1b7d9f2119`.

For a controlled quota comparison, `/etc/tmpfs-shm-wide` optionally allows
the desktop shared-memory mount to use three quarters of the existing global
tmpfs budget, instead of half. It does not reserve memory or increase that
global budget, and it leaves the default and the separate QEMU-only unbounded
mode unchanged. This is an experiment, not a remedy for stale objects or a
claim about Steam's freeze. With the separate larger-budget marker also set,
an offline 2-GiB QEMU guest reported a 372,465,664-byte desktop quota and a
496,619,520-byte global quota and passed the 480-case full-unlink probe.

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

Tmpfs backing pages and canonical mappings now share one PMM frame. The inode
index and canonical cache retain independent page tokens, and the payload quota
is retained until the last token is released. Cache metadata remains separately
charged. Private mappings preserve copy-on-write. Since mapped stores already
update the backing frame, tmpfs writeback updates metadata without replaying a
snapshot that could overwrite a newer mapped store. Other backends retain their
existing snapshot writeback. Module ABI is 65; the kernel and all modules must
be rebuilt and deployed as a matching set.

A sparse page is allocated before publication, so writeback requires no extra
persistent quota. Allocation failure unwinds the frame, page metadata and
quota. Defaults remain 16 MiB per instance and 256 MiB globally; the quota does
not account for private COW memory. Coherent tmpfs flushes no longer allocate a
snapshot; a mixed writeback pass can flush tmpfs even if allocation for a disk
backend fails, while leaving the latter queued for retry.

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

## Dell wider shared-memory comparison, 2026-11-04

An opt-in `/etc/tmpfs-shm-wide` marker changes the desktop shared-memory share
from one half to three quarters of the existing aggregate tmpfs budget; it does
not enlarge that budget. The default is unchanged. The exact Dell kernel and
initrd were first booted in QEMU, and the previous Dell boot slot was retained.

On the Dell, the resulting `/dev/shm` quota was 1,487,568,896 bytes. Thirty
samples five seconds apart found a minimum of 231,550,976 bytes free. An earlier
run with a 991,711,232-byte quota reached approximately 1 MiB free and logged
an out-of-space error. These runs were not synchronized and application traffic
can vary, so this is evidence of improved headroom, not a controlled proof of
the cause of the Steam freeze.

In the wider run, the Steam log contained no out-of-space, corruption or
unresponsive messages when checked after sampling, and Stardew reached its
title screen. The game was clipped inside the nested X server; neither this
observation nor the absence of log messages proves sustained interaction or
production readiness. Steam's tracked job later peaked at 243 of its 512-task
allowance with no denied task births. Its silent diagnostic audio setup was
unchanged. The quota experiment remains opt-in pending broader validation.

## Sparse-fault quota rollback

A failed mapped fault under quota or physical-frame pressure no longer leaves
a newly materialized, unused backing page charged to tmpfs. Canonical quota,
metadata and frame are obtained first; backend preparation failure releases all
three. This preserves pre-materialization before the page is exposed. A later
backend read error can still leave a backing page; this change does not unify
the two physical copies or prove a cause of Steam's freeze.

The tmpfs, file-object, mapping-pin and mapping-range host tests pass, including
a constrained-quota case that would formerly retain a backing page and index.
The matched kernel and initrd reached the desktop in a private QEMU boot. A Dell
clean restart into the staged candidate was requested, but the debug link did
not return; hardware acceptance remains pending. Astra reviewed the ordering
and rollback; it noted that concurrent quota and PMM exhaustion may now return
ENOMEM first.

## Shared-frame implementation, 4 November

A private QEMU guest booted the matched ABI-65 kernel and modules and ran
`scripts/workloads/tmpfs_coherent_probe.c`. A touched 26,214,416-byte pool
charged 26,865,664 bytes including metadata. The probe passed shared mapping
and file I/O coherence, msync, ordered cross-process visibility, private COW
and unlink/close. It does not establish real Steam stability or measure final
reclamation. Host tests cover low quota, retained page-token accounting, failed
first allocation, EOF extension, and both explicit and global flush without
backend data writes. Four focused host tests passed. Astra reviewed the frame
lifetime and locking.

The Dell is offline after the preceding checked reboot; the new kernel has not
been deployed there. After fixing the platform-power test's missing include
path, the complete host suite built. It passed 304 of 306 tests in parallel; the
scheduler-lock and audio-delivery timeouts passed individually on rerun. The
tmpfs and file-object tests also passed GCC ASan/UBSan with leak detection
disabled.

A subsequent change removed the unused heap snapshot from coherent tmpfs
flushes. Host tests inject heap allocation failure for both direct and global
flush, including a mixed disk/tmpfs queue; disk writeback remains queued while
tmpfs retires. Both affected tests pass ASan/UBSan. The native Steam-sized
coherence probe also passed on the updated kernel in private QEMU. The Dell
remains offline, so these results do not establish real Steam behavior.
