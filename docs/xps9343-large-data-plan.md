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
