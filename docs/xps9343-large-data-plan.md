# Dell larger data partition

The requested target is a small EFI partition and one large writable data
partition, so applications including Steam can use the SSD. This is a plan,
not evidence that the current kernel can mount such a partition. The current
review initrd's ESP selector specifies start LBA 2048 and 12,582,912 sectors
(6 GiB); this expected identity and span still need to be checked against both
live GPT copies before any migration. An earlier hardware inventory identified
a 500,118,192-sector SSD; that inventory predates installation and does not
prove the present GPT layout or free space.

## Current boundaries

- `ahci_system_volume.cpp` selects the ESP using an identity-bound record and
  strict GPT validation; boot update slots and recovery files are on FAT.
- `writable_desktop.cpp` mounts `/nix` from a leased FAT-backed `STORE.IMG` and
  binds persistent home. Its record, image bounds and final clean/dirty protocol
  are tied to that backing. FAT32 also limits an individual file to under 4 GiB.
- `writable_desktop_plan.h` admits at most 32 ext2 block groups and validates a
  fixed-size descriptor table. A large direct ext2 partition needs a separately
  bounded table, overflow-safe geometry and correct reservation of every backup
  descriptor-table block. Simply relaxing the group count is unsafe.
- Current SSD installer admission expects a single ESP. It is not a general
  two-partition migration tool. Direct partition writes need exclusive device
  ownership and protection against overlapping raw writers and duplicate mounts.

## Staged implementation

1. Read and validate both GPT copies and the disk identity on the live Dell,
   without modifying it. Verify the ESP, all occupied spans, usable range and
   independently bootable USB recovery. Preserve the current store and boot
   generation. Do not infer free space from the old pre-install inventory.
2. Implement explicit, boot-pinned selection of a data partition by disk GUID,
   partition GUID, type and exact span, plus filesystem UUID and size. Validate
   it against the same strict GPT snapshot used to select the ESP. A mismatch,
   dirty filesystem or uncertain write must select maintenance, never another
   disk or a silent writable fallback.
3. Add large-ext2 validation and direct partition-backed desktop admission,
   including bounded memory use, metadata overlap and backup-GDT checks,
   exclusive writer ownership, shutdown ordering and offline repair handling.
   Preserve the existing FAT-image path for the previous boot generation.
4. Build a migration tool that creates the data partition only in validated
   unallocated space, copies the stopped, clean store, verifies ownership and
   contents, and checks the new filesystem offline. Rehearse power interruption,
   both GPT copies, dirty refusal, recovery, persistence and two clean boots on
   separate full-capacity QEMU fixtures before touching the physical GPT.
5. Initially retain the existing 6 GiB ESP and activate the direct-store boot
   only as a new A/B generation. Keep the original STORE and generation for
   rollback. Once accepted, separately evaluate a smaller rebuilt ESP; reducing
   its GPT span alone would truncate FAT, and retained boot selectors also embed
   the old span. User-approved recovery must remain independently bootable.

Steam's signed-in images and proprietary game assets stay private and are not
part of Git or a public distribution. Check space and migration behavior before
copying them; current live `/nix` had roughly 507 MiB free during the review.

An unconnected geometry helper now calculates bounded group counts, descriptor
table blocks and sparse-super backup reservations for the proposed 4 KiB ext2
format. Host tests exercise the single-to-multiple descriptor-block boundary,
partial groups, forged geometry and the configured group limit. A read-only
check against a 17 GiB sparse `mke2fs` fixture with a two-block descriptor table
passed, as did `e2fsck -fn`. This establishes geometry only: the helper is not
wired into boot or a complete bitmap/inode-table validator, and has not been
tested on the Dell.

`bash scripts/test_large_ext2_helpers.sh` recreates the sparse 17 GiB fixture,
runs the geometry/header and timestamp host tests, and checks the fixture with
`e2fsck -fn`. It deletes only its own temporary files on exit.

The unconnected helper also checks each classic group descriptor's bitmap and
full 128-byte-inode-table placement and overlap, plus basic counter bounds.
Synthetic corruption and partial-group tests pass, as does the 17 GiB fixture.
It does not inspect bitmap contents or reconcile free counts, and it requires
the superblock's format and feature restrictions to be established separately.

An unconnected header classifier now checks the expected byte length and UUID,
the constrained ext2 feature and inode geometry, and dirty versus clean state
before returning the geometry for descriptor inspection. Synthetic invalid and
dirty cases and the 17 GiB fixture pass. This deliberately retains the existing
128-byte-inode restriction; that format has timestamp limitations beyond 2038,
which need a separate design decision before it is chosen for permanent storage.
The classifier alone neither verifies the partition identity nor proves
filesystem consistency, and no boot admission uses it yet.

The current ext2 driver calculates inode offsets using the on-disk inode size,
but reads and writes only the first 128 bytes. Its timestamp implementation
uses unsigned whole seconds, saturating at `0xffffffff`; that does not establish
Linux-compatible timestamps past 2038. A 256-byte inode alone is insufficient:
the driver would have to initialize, read, preserve and update extended timestamp
fields correctly, including on inode reuse, and validate their presence. Test
Brook/Linux round trips around 2038 and 2106, negative times and nanoseconds
before admitting that format. The legacy-format classifier stays restrictive.

Separately, `Ext2InodeSize` currently returns only the lower 32 size bits, even
though regular-file inodes have a high-size field. A large partition must not be
advertised as safe for files over 4 GiB until read, write, truncate, allocation,
stat and independent filesystem-check behavior have been audited and tested.
Those checks are distinct from allowing a filesystem larger than 4 GiB.

An unconnected extended-time codec now implements the signed base, two epoch
bits and nanoseconds described by the [Linux inode format](https://docs.kernel.org/filesystems/ext4/inodes.html).
Host tests cover epoch transitions, range rejection and round trips; this does
not add 256-byte inode support or change the mounted filesystem. An independent
`debugfs` fixture produced the expected raw base/epoch fields at the 2038 and
2106 boundaries, and `e2fsck -fn` passed. However, its displayed dates for
some raw values differ from the documented signed-base interpretation; current
e2fsprogs' [`ext2fs_inode_xtime_get`](https://github.com/tytso/e2fsprogs/blob/v1.47.4/lib/ext2fs/ext2fs.h)
passes the unsigned base to its decoder. The actual Linux mount/runtime and
Brook-to-Linux round trips remain untested; an e2fsck pass does not establish
timestamp interoperability.

An unconnected direct-data selector parses a bounded record for the disk,
partition, span and expected filesystem UUID. Its read-only GPT selection
requires matching validated primary and backup tables, Linux filesystem type,
disk and partition GUIDs, and exact start and length. Synthetic malformed,
overlap and duplicate-GUID cases pass, along with a separate two-partition
`sfdisk` fixture. The selector does not yet verify ATA identity, bind the ESP
selection to the same snapshot, compare the filesystem UUID with ext2, mount
anything, or authorize a write; those remain integration requirements.
An unconnected composition now also requires the ESP and data selectors to
match the same supplied GPT snapshot before publishing either result. Tests
cover mismatched identities, failure of the second selection, output aliasing
and an independently generated two-partition fixture. The caller must still
pin the disk layout and buffers until mount admission; the helper does not
enforce that lifetime or exclusivity itself.
An offline inspection composition additionally matches the selected span to
the ext2 byte length and UUID, distinguishes dirty headers, and validates all
group descriptors before reporting a clean format. Host synthetic cases and
independent 16 MiB and 17 GiB GPT/ext2 fixture pairs pass. These pairs are
separate files and do not exercise partition-relative reads, disk ownership,
mounting, recovery or filesystem consistency; the composition remains unused
by live boot.

The first independent GPT test actually failed: the selector and synthetic
fixture shared a wrong byte order for the Linux filesystem type GUID. The test
command's exit status was overlooked. The selector and fixture are corrected;
the small and 17 GiB separate fixture pairs now pass with exit status checked.
`scripts/test_ahci_data_selector.sh` creates an independent GPT with `sfdisk`
and formats ext2 inside its data partition with `mke2fs`, then validates the
embedded filesystem at partition-relative offsets. The same check also passed
on a sparse 18 GiB disk with a 17 GiB data partition. These are host-side file
reads, not AHCI device, mount, filesystem write or durability tests.

An unconnected exact-read adapter now obtains both GPT copies and, after
selection, reads the superblock and descriptor table relative to the chosen
partition. The embedded fixtures exercise the adapter as well as the pure
inspection: injected errors and short reads fail closed at each of seven clean
read steps, and an injected dirty header returns an offline-check candidate
without reading the descriptor table. The caller must still pin disk identity,
layout and contents; these host-file callbacks do not test an AHCI controller,
exclusive ownership, mounting or writes.

An opt-in boot diagnostic now invokes that reader only after the existing
system-volume admission succeeds and `/etc/ahci-data-inspect` supplies a valid
selector. It reports a format-eligible header, offline-check requirement or
rejection; it does not mount the data store or change the desktop fallback
decision. The ordinary boot has no such marker. Host tests cover absent and
invalid selectors, each scratch-allocation failure, a device-read failure,
dirty and valid fixtures, with no writes or additional mounts. The kernel
builds. A failed hardware read can still quarantine the existing AHCI device,
so read-only inspection is not necessarily operationally harmless.

The diagnostic was subsequently boot-tested in private QEMU on a cloned,
snapshot-backed Dell-capacity disk: a 6 GiB ESP retained the existing store,
and a separately formatted 17 GiB ext2 data partition was identified as entry
2 with 136 groups. The existing store, network and desktop also started; a
checked shutdown passed. A second boot with a deliberately mismatched data
filesystem UUID reported rejection while the existing store and desktop still
started and shutdown passed. These tests did not mount or modify the data
partition and do not establish AHCI ownership, writable behavior or Dell
hardware compatibility. The first rehearsal used obsolete module ABI 57 with
kernel ABI 63, so networking failed and that VM was stopped; matching modules
were used in both successful boots. Results and serial logs are in
`build/hardware/data-probe-qemu-run2-20261102` and
`build/hardware/data-probe-qemu-reject-run-20261102`.

The proposed large-data writable QEMU test has a separate, currently
unconnected admission predicate. It requires KVM, an explicit marker, no
conflicting test markers, an exact QEMU model/serial/capacity, 512-byte logical
and physical sectors, and flush support. Host negative tests reject the Dell's
physical model and altered identity or geometry. This predicate does not enable
the existing raw-write or installer tests and does not itself open a disk.
Integration must compile-gate every caller and reject conflicting markers
before evaluating any test-mode branch, not just this predicate.

A distinct compile-gated QEMU mode now uses that admission to mount the
inspected data partition at `/mnt/ahcitest`, without enabling the legacy
fixed-offset raw-write test. Two snapshot-backed QEMU boots confirmed that a
wrong disk serial and conflicting markers both leave the disk read-only and
refuse the old raw-write hook. On a separate writable private clone, a 17 GiB
ext2 partition mounted through AHCI; the test created, fsynced and renamed a
150,123-byte file, completed checked shutdown, and verified the file after a
fresh boot and second checked shutdown. A sparse host extraction passed
`e2fsck -fn`, and its file content matched an independent host oracle. GPT
metadata and a selected gap sentinel were unchanged. Evidence is under
`build/hardware/large-data-write-run-20261102` and the two
`large-data-negative-*-run-20261102` directories.

The QEMU-only allocator was then configured to prefer group 128 for this
identity-gated test device; the production kernel retains its original
first-fit loop. A fresh private fixture completed create, checked shutdown,
reboot verification and a second checked shutdown. Offline extraction passed
`e2fsck -fn` and the independent file-content oracle; `debugfs` reported all
38 file blocks in group 128. This exercises block allocation through the
second descriptor-table block, not inode allocation in group 128. An earlier
attempt completed its create phase but its QMP socket path was too long; it
was not counted as a passing reboot test. Evidence: `build/hardware/ld2`.

A further fresh fixture also preferred group 128 for inode allocation in the
QEMU-only mode. The test file survived create and verify boots with checked
shutdowns; offline inspection found inode 8193 with 64 inodes per group and
all 38 allocated blocks in group 128. The independent payload comparison and
`e2fsck -fn` passed. This does not cover late-group inode reuse, directory
counts, full-group fallback or interrupted-write recovery. Evidence:
`build/hardware/ldi`.

The QEMU-only path now rechecks the large store and marks it dirty with a
completed flush before mounting. Following successful global VFS sync with
write admission closed, its shutdown hook marks the store clean and flushes.
Both create and verify boots reported the dirty and clean transitions; the
extracted filesystem passed `e2fsck -fn` and an independent content check
(`build/hardware/lds2`). A separate deliberately dirty clone was refused before
the test mount while the existing desktop store started (`build/hardware/lddirty`).
Host tests cover read, partial-write and flush failures and confirm that failed
global sync skips clean finalization. A failed clean flush can leave an
uncertain on-disk clean state; it is reported as failure and not retried, not
guaranteed to be detected dirty on the next boot.

This path still does not provide production partition ownership or a production
mount. Its marker does not enable this mount in a production kernel. No Dell
SSD access was performed by these tests.
