# Dell XPS 13 9343: internal SSD installation preparation

The user has explicitly
authorized wiping the old Ubuntu contents and installing Brook on this Dell's
internal SSD. Physical writes still require an implementation that targets the
observed SSD, preserves USB recovery, and passes the applicable tests below.

## What is known

- Brook booted the Dell from its existing USB stick. The tested A/B USB payload
  and root recovery path are the current way back into Brook.
- A read-only AHCI probe identified a Samsung PM851 M.2 2280, 256 GB, on Intel
  8086:9c83 SATA port 3: 500,118,192 logical sectors of 512 bytes. Record the
  exact ATA serial and physical sector size privately from a fresh boot; do not
  infer them from this model name or commit them to Git.
- The last read-only inventory found a legacy MBR with one active Linux
  partition. This was not a filesystem health check. The SSD has not been
  written by Brook.
- A disposable QEMU SATA disk with the same logical capacity received a 6 GiB
  desktop partition and booted without USB. A separate clone passed two checked
  boots and application-store persistence plus offline GPT/FAT/ext2 checks.
  The persistence clone had its test helper replaced. Browser interaction,
  physical SSD behavior and arbitrary power-loss recovery were not established.
- A small QEMU install interrupted during payload copying was successfully
  restarted from USB. QEMU process exit is not proof of a physical power cut.
  That interruption test exercised the synthetic-device admission only.
- The explicitly enabled physical-installer kernel now reserves the exact
  identity-matched disk outside the normal registry, requires a boot-pinned
  package digest and a boot-bound start key, and shares the shutdown gate.
  Its full-size disposable QEMU installation passed independent comparisons of
  the GPT and partition ranges. The installed disk booted without USB and
  passed checked shutdown, restart, store persistence and filesystem checks.
  A copy of the Dell candidate, with only the identity adapted for QEMU and
  its test helper replaced, passed the same checks. Neither is physical SSD
  write/flush acceptance or proof of arbitrary power-loss recovery.

## Remaining engineering gates

1. **Backup decision.** The user explicitly waived backing up the old Ubuntu
   SSD. A physical read-only 64 MiB sample transferred at about 1.13 MiB/s;
   extrapolating gives approximately 60 hours for a full image, without
   accounting for verification or variation. No full SSD backup exists and
   installation will destroy the old data. The separate USB store backup is
   retained. If a backup is subsequently requested, preserve the entire
   256,060,514,304-byte logical device, not just its partition table. A raw
   backup needs at least that much free destination space plus overhead; the
   host `/data` had roughly 290 GiB available after cleanup; recheck the space
   immediately before starting because it can change. A rescue environment must
   disable automount and mark the SSD block device
   read-only before inspection or imaging. Record source identity, size, read
   errors, backup length and a full-image digest in private local evidence.
   Hash the complete source stream during acquisition, then independently read
   back or decompress the stored backup and require the same byte count and hash.
   Unreadable sectors require explicit handling and cannot be called a verified
   full backup. Test the restore workflow against a disposable image. Record
   UEFI boot mode, settings and relevant boot entries separately: disk imaging
   does not preserve firmware NVRAM. The opt-in root-only AHCI export and bulk
   `disk` action stream an entire read-only disk without staging it in guest
   RAM. The host requires a separately observed same-boot identity and a size
   ceiling, checks free space, validates the guest receipt, and independently
   rereads the file before publication. A disposable 64 MiB QEMU disk matched
   byte for byte. A fresh regular-file copy of that backup also booted in QEMU
   with no USB storage attached and the base image unchanged
   (`build/hardware/backup-restored-boot-qemu-20261031`); this is kernel boot
   only, not a full desktop test. Physical-disk acquisition and restore have
   not been tested.
   This is not an atomic snapshot against changes outside Brook's read-only
   admission, has no resume or automatic retry, and aborts on read error. Use a
   private output directory and preserve its receipts: the image and identity
   may contain personal data. An independent rescue environment remains an alternative.
   A backup is for recovery, not a claim that a wiped SSD has been
   securely erased.

   For a QEMU rehearsal, capture `bridge hardware ahci-ro` from the pinned
   boot, extract exactly `sectors`, `logical_sector_bytes`,
   `physical_sector_bytes`, `port`, `model` and `serial` into a private JSON
   identity file, then invoke `scripts/hardware_transfer.py --action disk`
   with `--session`, `--key-file`, `--disk-identity`, `--max-bytes` and a new
   private `--output` directory. Use the forwarded `--port` for QEMU. The
   disk image and receipt will be inside that directory. Inspect the same
   transaction if it fails; do not automatically restart it. A successful
   guest receipt alone is insufficient without the completed host readback.
   During transfer and the independent readback, `receipt.json` records phase
   and byte counts at approximately ten-second intervals. Transfer counts are
   progress indicators, not proof that the data has been durably verified.
2. **Build a physical candidate.** A private target-sized GPT and boot
   payload has been generated using the Dell's observed controller, port, model, serial,
   capacity, sector sizes and flush capability. The QEMU desktop image embeds
   `QEMU HARDDISK` and a synthetic serial; it must never be reused as the Dell's
   write-target record. Generate new disk/partition identifiers and a matching
   boot-volume selector together. Keep those and per-device records private.
   The initial 6 GiB partition contains a 3.75 GiB store copied from the
   repaired USB snapshot; further partition/store expansion remains future
   work. FAT32 cannot contain a file of 4 GiB or more.
3. **Installer admission and recovery.** The explicitly enabled physical path
   implements
   exact physical target binding, source-device ancestry and exclusivity,
   immutable source leases, a trusted package identity and pre-write verification
   of the leased bytes and geometry. Physical recovery and durable install
   progress after a lost boot remain unproven. The v2 package checksum detects data changes but a
   record stored with the package is not authentication. Refuse ambiguous
   targets, unverified packages, unsupported sector sizes, missing flush support
   and uncertain I/O. Preserve the USB's working A/B and root recovery files.
   The test installer now verifies source partition ancestry against the
   registered USB parent rather than trusting a device name; host tests and a
   complete disposable QEMU install passed. This is not proof of physical USB
   hotplug handling or SSD write/flush behavior.
4. **Rehearse the exact candidate on disposable media.** Use the full desktop
   package and a QEMU disk of the same sector geometry, with the intended target
   and boot selectors adapted only to a synthetic identity. Verify negative
   identity/geometry/package cases perform zero target writes; compare installed
   ranges independently, boot without USB, exercise the actual browsers and
   Gallery, write and sync user data, shut down and verify it after a fresh boot.
   Test interruptions at multiple phases and a separate recovery boot. A QMP
   quit is only a guest-interruption test; do not claim it proves persistence
   under host power loss.
5. **Recheck the Dell and recovery media.** Confirm the target identity and
   health, copy and verify the candidate on suitable storage, remeasure free USB
   space, and confirm the original USB recovery boot still works. The previous
   USB free-space observation was about 30.3 GB and may be stale. The Dell must
   be reachable and physically observable. Keep the existing USB recovery
   files and the separately repaired store snapshot.

## Eventual installation and acceptance

Once the applicable gates above are satisfied, use the
reviewed installer for the uniquely identified SSD. Check its receipt and
readback, then boot the SSD with USB removed while retaining the USB recovery
stick. Verify UEFI boot selection, the expected kernel and store, browser and
Gallery interaction, persistence through checked shutdown/restart, and that a
second cold boot works. If any stage is uncertain, stop rather than replaying a
write: retain the disk and receipts, boot the independent recovery media, and
inspect whether the verified backup must be restored. A restore procedure must
also identify its destination and verify written bytes; it is not implemented
yet. Do not claim automatic rollback, secure erase or crash consistency.
