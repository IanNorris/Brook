# Growing BROOK_SYSTEM on the desktop SSD

The desktop boots from the external Lexar SSD: p1 BROOK_RTX (FAT32 boot volume),
p2 BROOK_SYSTEM (ext2: /nix and home), p3 Ian's NTFS "Backup". BROOK_SYSTEM was
made 128 GiB, leaving about 342 GiB unallocated before p3. Growing it into that
gap gives room for large Steam games.

Brook pins p2 exactly. The initrd's `/etc/usb-system` names p2's GPT GUID, start,
size and ext2 UUID, and `usb_system.cpp` refuses to mount unless the GPT entry
and the ext2 superblock match it exactly (the ext2 must fill the extent:
`LargeExt2ClassifyHeader`). After a grow, the old desktop set on p1 is refused
with `gpt-identity`. A set built from the new record must be installed in the same
session, before the SSD goes back to the desktop.

## Capacity

The grown store has 3759 groups of 128 MiB, which is 469.875 GiB.

- Brook admits up to 4096 groups (512 GiB): `LargeExt2GeometryMake` and the
  4096×32-byte descriptor buffer in `Activate`. The record must end within
  2^32 sectors (`UsbSystemParse`). The SSD has 3,907,029,168 sectors.
- resize2fs grows the descriptor table to 30 blocks without `resize_inode`, and
  moves the bitmaps and inode tables in the backup groups. The admission
  descriptor checks accept that layout. `test_large_ext2_geometry IMAGE` was
  run on the grown fixture.
- The trim scan, statfs and allocator use 64-bit or per-group arithmetic.
  Block and inode bitmap caches are allocated per group as they are used, up
  to about 15 MiB of block bitmaps if every group is touched.
- **Files can be about 2 TiB** (work/ext2-large). Regular-file sizes use
  `i_size_high`, and writes, reads and truncates go through triply indirect
  blocks. The limit is Linux's `ext2_max_size` without HUGE_FILE: 2 TiB less
  about 2 GiB at 4 KiB blocks, set by the 32-bit `i_blocks`. Writes stop at
  that limit with EFBIG, and so does a truncate past it.
  `fallocate` still returns EOPNOTSUPP on ext2 files.
  Host proof: `test_ext2_objects`. QEMU proof:
  `scripts/test_ext2_large_file_qemu.py`.

## Procedure (on the build host, SSD attached by USB)

1. Unmount anything the desktop auto-mounted from the SSD (p1 BROOK_RTX, p3
   Backup), and confirm the disk name with `lsblk -o NAME,SIZE,TRAN,MODEL,PARTUUID`.
2. Dry run (writes nothing; prints the plan, runs `e2fsck -f -n`):

       sudo bash rtx-smoke/grow-brook-ssd-system.sh /dev/sdX \
           rtx-smoke/store-bd024306/ssd-system.json --dry-run

3. Grow (asks for `GROW BROOK SYSTEM sdX TO 469 GIB`):

       sudo bash rtx-smoke/grow-brook-ssd-system.sh /dev/sdX \
           rtx-smoke/store-bd024306/ssd-system.json

   It writes `rtx-smoke/store-bd024306/ssd-system-grown.json`: same GUID, start
   and ext2 UUID, with new `sectors` and `fs_blocks`. The old record is kept.
4. Build the desktop set with the same builder arguments as the current
   desktop set, changing only `--usb-system` to the grown record:

       nix-shell --run "python3 scripts/build_rtx_smoke_files.py ... \
           --usb-system /data/Programming/Brook/rtx-smoke/store-bd024306/ssd-system-grown.json ..."

   The builder refuses a record whose `fs_blocks` does not fill its extent.
   Check the set: `mtype -i FILES/INITRD.IMG ::/ETC/usb-system` must show
   `2752512,<new sectors>`. A compressed initrd needs expanding first.
5. Install it (asks for `WRITE BROOK BOOT sdX`):

       sudo bash rtx-smoke/write-brook-ssd-boot.sh /dev/sdX FILES_DIR \
           rtx-smoke/store-bd024306/ssd-system-grown.json

   `write-brook-ssd-boot.sh` needs no change. It reads the extent from the JSON
   and checks it against the GPT and the set's `/etc/usb-system`.

If step 3 stops before resize2fs, the script puts back the old p2 entry. If it
stops at or after resize2fs, keep the grown entry, run `e2fsck -f -n` on p2, and
get help before retrying.

## Rehearsal and QEMU evidence

- `rtx-smoke/grow-rehearsal/rehearse.py` builds a sparse Lexar-layout disk
  with the real GUIDs and extent. p2 is a reflink of the QA 128 GiB store, and p3
  is a 64 MiB NTFS "Backup". The scripts run as root in a throwaway Linux VM that
  sees the disk as USB mass storage with model "Lexar SSD NM620".
- `scripts/test_usb_system_grow_qemu.py` boots Brook on the grown disk through
  the usb-system path. It writes a file whose blocks are all past the old
  128 GiB end, reads it back after a restart, power-cuts the guest, and times
  the boot-time e2fsck repair. It then checks that the GPT apart from p2, p1's
  files, p3 and the backup GPT are unchanged.
