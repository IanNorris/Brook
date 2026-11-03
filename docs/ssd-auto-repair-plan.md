# SSD store automatic repair

**Current Dell layout:** the installation now uses a direct ext2 data
partition. The earlier FAT-backed image design below describes the older
layout and is not an implementation plan for the current SSD. The same
durability and exclusive-ownership requirements apply, but identities must
bind the physical disk, GPT disk and partition GUIDs, partition bounds and
ext2 UUID. No automatic writer or recovery admission is implemented yet.

## Experimental boot guard

An opt-in build produces a separate `BROOKGUARD.efi`; the normal
`BOOTX64.efi` is unchanged. The guarded binary embeds its expected disk GUID,
data partition GUID and ext2 UUID and requires two identical, valid, clean
records on the EFI system partition before trying any normal A/B or legacy
payload. An absent, interrupted, inconsistent or wrong-identity record enters
the independently verified recovery pair; if that pair is unavailable, the
loader halts. The decision does not depend on `BROOK.CFG`.

On a private QEMU SATA fixture, the clean case booted normally and pending,
mismatched, missing, truncated, corrupt and wrong-identity records selected
recovery, including when the configuration file was absent. These checks used
the legacy normal payload. Follow-up tests exercised a corrupt newest slot
with a usable older slot and invalid selectors with a usable legacy payload;
clean guards allowed the respective fallback, while pending guards selected
recovery. With a pending record and either a missing recovery initrd or a
corrupt recovery selection, the loader reached its fatal recovery message
without a normal kernel marker. Each denied case ran on a writable private
copy and left its full disk hash unchanged. Interruption across durable writes
still needs acceptance tests. The guard has no writer and
does not itself validate the physical disk or filesystem. It is not installed
on the Dell. Before any physical deployment, inventory every firmware-reachable
EFI entrypoint: an older EFI loader can bypass this guard even though old
kernel generations loaded through the guarded EFI cannot.

An unintegrated transaction primitive now specifies the first repair step:
validate both clean records and the expected identity, then write, sync and
independently read back a higher-generation pending record in each slot. It
authorizes a checker only after both records have been verified; any failure
after a write attempt is treated as uncertain. Host fault tests model failures,
partial writes and early persistence at every operation. This is not yet a
working disk transaction. A dedicated fixed-record FAT lease now accepts an
existing 128-byte guard while retaining the image API's sector-alignment
requirements. Host FAT16/FAT32 tests cover sole ownership, aliases, fixed-size
writes and sticky flush failure. A guard-specific sync/readback operation now
reads the retained file's mapped sector through diskio instead of the FatFs
file buffer. Host tests cover stale file-cache data, failures, malformed maps,
sector-tail preservation and a nonzero partition offset. This verifies the
Brook/FatFs cache boundary; it does not prove SSD controller cache or physical
power-loss behavior. A retained-pair validation also rejects distinct guard
entries that share a cluster, tested with cross-linked on-disk FAT16/FAT32
fixtures. This only compares the two guards: it does not detect links to a
third file or establish exclusive ownership of the ESP. Those remain necessary
before the transaction can be wired to a checker or deployed.

A host-only adapter now exercises the pending transition against the real
FatFs lease, pair check and direct readback on FAT16, FAT32 and an offset FAT16
fixture. It verifies matching pending records after reopen, EFI guard denial,
and sticky lease failures after injected writes, syncs and reads on either
record, plus failure of the final drive flush. The host
backend writes to a memory-backed disk without modelling volatile SSD caches
or physical power loss. Its exclusive fixture is not a production ESP
ownership mechanism.

An experimental boot-only preparation path can validate the selected GPT
layout and reserve the ESP for a dedicated block adapter while denying data
partition and GPT metadata writes. The adapter itself currently rejects all
writes, and the writable private FAT mount rejects this reserved device until a
checker-completion and ownership-handoff gate exists. Preparation itself is called
only by the isolated check-only boot; it mounts no FAT filesystem and does not
run a repair. A later read-only diagnostic mount is described below.
Host tests exercise read-only routing,
bounds, failure and repeated admission. A separate one-shot, boot-lifetime FAT
binding now prevents the same device pointer from being bound or mounted via
another FAT slot; host tests cover competing binders, release and sticky write
errors. An unwired private mount prototype uses that binding to retain and
inspect only the two existing guard files, without publishing a VFS namespace
or exposing writes. FAT fixture tests use a stubbed binding layer, verify no
sector writes, and cover partial failure and mutual crosslinks. They also
demonstrate that a third file crosslinked to a guard is not detected. These primitives do not
prevent trusted kernel code from using diskio directly, prevent different
adapter identities from overlapping the same blocks, or detect guard files
cross-linked to a third file. The path must not be activated after any
disk-backed filesystem or cache has been started.

Guard preparation also reads the selected partition's primary ext-family superblock
and requires its magic and UUID to match the configured data identity before
installing the reservation. A dirty state is allowed; an unreadable, damaged
or mismatched primary identity is rejected for manual recovery. This is an
identity check, not a filesystem integrity check, feature/geometry validation,
an ext2-versus-ext4 distinction, or backup-superblock repair.

An opt-in `BROOK_SSD_GUARD_CHECK` kernel now calls that reservation immediately
after AHCI initialization, before other storage consumers. This dedicated
build skips storage auto-mounts, deferred probing, page stores, late modules
and normal system/data mounts; reservation failure halts the boot. It cannot
be combined with installer or writable-test hooks. On disposable QEMU SATA
fixtures, the expected identity reserved successfully and an incorrect ESP
GUID was rejected; full disk hashes were unchanged. A second virtio disk
requesting a `/boot` mount did not replace the RAM initrd or change either
disk. The mode cannot repair a disk, and matching physical targets are not
kernel-restricted to QEMU. It is not installed on the Dell.

The dedicated kernel now starts a checker controller instead of the ordinary
boot script. It hashes the exact loaded static FAT checker against a pinned
digest, rejects an interpreter, binds the read-only session before scheduling
the checker, and reports its kernel-observed exit after revoking access. The
debug channel is disabled during this boot. On disposable QEMU disks the
checker returned 0 for a clean ESP and 1 for crosslinked files; a modified
checker was rejected, with disk hashes unchanged. This is diagnostic only:
it does not mount the ESP, repair ext2, authorize a normal boot, or establish
physical power-loss safety. If a checker times out and cannot be reaped, the
session stays revoked and recovery remains incomplete.

After a clean checker result, the diagnostic now also checks a deliberately
restricted FAT16/FAT32 geometry, both complete FAT copies, FAT32 FSInfo bounds
and the backup boot sector while the same reserved adapter remains pinned.
The profile excludes the cluster-count boundaries on which the bundled
FatFs and checker disagree. Host tests compare accepted real FAT16/FAT32
fixture geometry with FatFs's mounted geometry and reject a nested MBR volume;
the check-only QEMU boot passes on its FAT16 fixture. This is not yet an
authorization to write. A diagnostic-only private read-only FatFs mount now
compares its actual geometry with the profile and reads both guard records,
checking them against the disk/data GUIDs and verified filesystem UUID. It
never publishes a namespace or makes the adapter writable. QEMU tests cover
matching, mismatched, missing and oversized records with unchanged disk hashes;
host FAT16/FAT32 tests cover read failures, incorrect mounted geometry,
one-shot behavior and cleared output. This binding remains read-only until
reboot and cannot serve as the later writable transaction handoff.

Separately, a default-off `BROOK_SSD_GUARD_PENDING_TEST` build exercises the
pending-record transaction end to end on a disposable QEMU SATA fixture. Its
driver requires KVM, a dedicated marker, exact QEMU controller/disk identity,
geometry and flush support in addition to the existing write-target check; the
host harness attaches only a new regular-file copy. A second private adapter
keeps writes disabled during checker execution and FAT preparation, enables
only owner-routed ESP writes during the one-shot transaction, then permanently
disables itself. GPT and data-partition writes remain reserved. This is not
compiled into the ordinary recovery kernel and cannot authorize ext2 repair.

The test boots the real checker, validates the FAT profile and both records,
writes and independently reads back the pending pair, and compares the full
disk against its original with differences restricted to the two guard data
areas and permitted directory timestamps/archive bits. A fixture-bound EFI
loader accepts the original clean records and selects verified recovery after
the pending transaction. Injected failures before each transaction operation
are checked against the exact expected record states and the subsequent EFI
choice. These are logical failure tests in QEMU, not physical power-cut or
controller-cache durability evidence. The test adapter and its safety checks
must not be mistaken for a production writable recovery handoff.

A further default-off `BROOK_SSD_GUARD_EXT2_TEST` build extends that disposable
path: after the pending records are durable, it grants the authenticated static
e2fsck process access to a separately reserved data-partition adapter. Preen
and verification use distinct, process-bound sessions; the verification session
rejects writes. A clean fixture and a deliberately dirty bitmap fixture passed
preen and a separate `-f -n` check; the latter reported preen exit 1. After
QEMU stopped, an independent host `e2fsck -f -n` passed, a known file and UUID
were intact, changes outside the data partition and permitted guard bytes were
rejected, and the guarded EFI boot selected recovery. Pending failure and
missing or altered checker tests left the data partition unchanged. Host tests
cover authorization, bounded I/O, separate verifier permissions, and blocked
write/flush cancellation. If a cancelled checker does not drain, the test does
not advance the adapter or verification. In this test records remain pending.

The separate, default-off `BROOK_SSD_GUARD_COMPLETE_TEST` adds a one-shot
completion after the checker is drained, the repaired data is flushed and
verified, and its adapter is permanently disabled. It requires both records to
match the exact pending record for this attempt before writing either clean
record. A deliberately dirty private QEMU disk passed repair, independent host
verification and subsequent guarded EFI normal-boot selection. Completion
failure injection checks the exact record pair and EFI selection; an
uncertain late failure can still leave a clean pair, after verification has
already finished. These are logical fault tests, not physical power cuts. The
normal boot was also tested with a compatible 4 KiB ext2 fixture and an
otherwise normal kernel: after an intentionally dirty filesystem was repaired,
the kernel admitted the direct data partition and a fresh process read the
known file through `/nix`. The fixture's application tree contains placeholders,
so this does not establish full desktop application behavior. Production
admission and hardware durability are not implemented or proven.

An unpublished kernel-only vnode can now present the prepared adapter as a
bounded, uncached, non-mappable regular file. Its backend rejects writes and
metadata changes, handles short/error reads, and is covered by host tests. It
has no user-space path. An unpublished one-shot checker session now restricts
opens and reads to a root-started process group and leader incarnation, and
counts outstanding views and reads. Abort rejects new access while admitted
reads finish; it does not authorize a mount or repair. Host tests cover races
and retained references with a stubbed VFS. Trusted executable and exec-lifetime
checks, actual checker launch and exit authentication, and an ownership handoff
remain necessary before this can be used to authorize a mount. A reserved
`/dev/brook-guard-esp` open route connects that gate to normal file descriptors;
it denies access while no session is active and rejects write, creation and
unknown open flags. A disposable QEMU recovery boot exercised denied opens and
verified that the full disk hash was unchanged. This does not test an
authorized checker, path aliases or a substituted node in another namespace.

The pinned Nix environment produced a static musl dosfstools 4.2 checker
(SHA-256 `99cc8160e7a076ecd9cb04e13bdbe71c4aad4c64bfdc43ea83a016c8fee9d5e3`).
It can be rebuilt with `nix-build --no-out-link -E 'let pkgs = import
./nix/nixpkgs.nix {}; in pkgs.pkgsStatic.dosfstools'` from the repository root.
On disposable host FAT16 and FAT32 filesystems, `fsck.fat -n
--variant=standard` exited zero for clean images and nonzero for a file
crosslink, divergent FAT copies and truncation, without changing the images.
In a private Brook/QEMU installer fixture the same static checker exited zero
on a clean ESP and reported the shared clusters with exit 1 on a deliberately
crosslinked ESP; full disk hashes were unchanged. The QEMU checker uses the
installer's writable-capable block interface and is diagnostic only, not an
admission gate. A production gate must retain exclusive ownership of the exact
ESP across checking and mounting, enforce a read-only checker interface, accept
only a completed exit 0, and constrain FAT geometry/interpretation. Further
fixtures and physical validation remain necessary.

Unintegrated bootloader helpers now validate a bounded GPT hard-drive device
path, an exact whole-disk parent-path match, matching primary and backup GPTs,
the ESP and data partition identities and bounds, and the ext2 header state.
Host tests exercise malformed paths and partition metadata. These pure helpers
perform no firmware I/O, are not part of the boot decision, and cannot grant
repair authority. Firmware handle selection, alignment and media-change
handling, independent identity binding, and real automatic dispatch remain.

The Dell boot image already contains a static `e2fsck`, but the normal boot
does not invoke it. A dirty store is rejected before `/nix` or `/data/home`
is published, and the system remains in the initrd maintenance environment.
The user has requested automatic repair; the procedure below is not yet
implemented.

Dirty-store classification validates the superblock identity and expected
geometry before returning `needs-filesystem-check`, but deliberately does not
require group descriptors to be valid. Their condition remains unknown until
the offline checker reads them. Clean-store admission still validates group
descriptors before any write or mount. A deliberately dirty private QEMU store
with an invalid block-bitmap pointer entered maintenance without mounting or
altering the store; host tests cover the corresponding clean-state rejection.

The repair must happen after the scheduler can run a user process: store
admission currently runs before ordinary user processes can execute. On the
verified system-volume path, a boot-only maintenance coordinator should:

1. Only when the bound SSD identity, `STORE.OK` and configured image agree,
   and admission ended specifically at `needs-filesystem-check`, retain the
   already-acquired `FatImageDevice` backing and exclusive FAT lease instead
   of destroying it. Keep it unregistered, unbound to ext2 and unavailable
   through ordinary pathname opens. Other failures remain maintenance failures;
   they must not trigger repair against a guessed disk or another file.
2. Before any checker writes, persist and flush an identity-bound pending
   repair record. Normal store admission must respect that record even if an
   interrupted checker has already marked ext2 clean. Suppress ordinary app
   startup on this repair boot and start the coordinator only once the scheduler
   and cleanup infrastructure can run it.
3. Provide the checker a narrowly scoped descriptor over the retained device,
   with bounded reads, writes, seek, stat and flush. Keep the exclusive lease
   across both checker processes. Run the bundled checker with `-p`; save its
   exit status and output in the maintenance console. Never run a second
   checker or admit the store concurrently. A lost helper or ambiguous status
   is failure, not permission to retry a write automatically. Timeout handling
   must terminate and drain the checker and its I/O before ownership changes.
4. Treat preen exit codes 0 and 1 as candidates for recovery,
   flush the backing file and its FAT volume, then run a separate read-only
   `-f -n` verification. The read-only check must exit cleanly. Interrupted,
   uncorrected, operational, usage and cancellation outcomes remain in
   maintenance; never turn them into a successful boot.
5. Clear the identity-bound pending record only after successful verification
   and checked flush; persist and verify its completion before releasing the
   repair lease. A clean reboot is the first implementation's admission path: it
   rechecks SSD identity and `STORE.OK` and avoids resetting the existing
   boot-once mount state. Limit automatic repair/reboot attempts using durable,
   verified state so a recurring failure cannot create an endless reboot loop.
   Surface the result to the user and keep the USB recovery path intact.

Before enabling this on the Dell, use disposable QEMU disk copies to test clean
boots, dirty-but-repairable and uncorrectable filesystems, mismatched identity
or readiness records, concurrent aliases, checker failures and timeouts,
interruption at each write/flush/reboot boundary, retry limits and independent
offline `e2fsck -fn` verification. A clean result must survive a second boot
and preserve a known pre-reboot file. QEMU interruption does not establish
physical power-loss behavior. Keep the accepted physical boot generation and
USB recovery available through the first Dell test.

## Checker interface probe

The exact bundled static e2fsck 1.47.4 was traced on Linux against a disposable
64 MiB ext2 fixture, first with `-f -n` and then after marking it unclean with
`-p`. In the preen run it opened the image with `O_RDWR|O_EXCL`, kept that
descriptor open, and opened the same pathname again read-only for geometry
probing. Its image I/O included `pread64`, `pwrite64`, `lseek`, `fstat` and
`fsync`; block-size/read-only ioctls returned `ENOTTY` for the regular-file
fixture and it continued. The clean-state repair performed a 4096-byte
positional write followed by syncs. Therefore the repair endpoint needs
controlled concurrent read-only opens by the authorized checker in addition
to its exclusive writable description, and should present regular-file
semantics rather than assuming one inherited descriptor is enough.

This is evidence for one small Linux-hosted fixture and one repair case, not a
complete syscall specification or proof that the synthetic endpoint works in
Brook. More involved repairs and the actual guest checker still need testing.
