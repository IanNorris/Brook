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
- The initial read-only inventory found a legacy MBR with one active Linux
  partition. That was not a filesystem health check. The explicitly authorized
  physical installation has since replaced the partition table and written the
  Brook image; the old Ubuntu contents were not backed up.
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
- On the Dell, the identity-bound installer verified the complete source,
  completed 196,624 copy, verification and metadata operations, and reported
  success with no error. A checked reboot selected the still-connected USB;
  it did not establish SSD boot. The guest reached a clean, safe-to-power-off
  state, but its power-off method is unsupported. A manual power-off and boot
  with USB removed was required for the first SSD boot test.
- The user subsequently reported that Brook reached the desktop from the
  internal SSD with the boot USB removed. The debug IP was initially
  unreachable; after the user restarted, DHCP succeeded. The kernel log then
  confirmed the GPT-selected SSD partition mounted at `/media/system` and the
  persistent home store was ready. An ordinary-user test wrote, synced and
  checked a unique file, then verified its contents after a checked restart;
  the restarted SSD boot reacquired networking. Settings was launched from the
  installed-app catalog and its window appeared in a captured completed frame;
  App Gallery also produced a window. Firefox launch was dispatched, but no
  window had been observed when the debug network became unreachable again.
  The reason for that loss and the earlier missing network on one boot remain
  unexplained; neither observation establishes that Firefox caused it. A prior
  USB-booted Dell run also lost network contact after a Firefox session (see
  the Firefox HTTPS follow-up in `xps9343-bringup.md`), so this is not yet
  evidence of an SSD-specific failure. Physical screen and input observations
  are needed to distinguish a guest hang from an adapter or link failure.
- The user subsequently confirmed that the desktop had hung without a
  watchdog response. After the forced restart, the first boot reported an
  xHCI controller reset failure; a subsequent boot had network access again.
  The SSD system partition mounted, but the store was refused at
  `header-and-dirty` and remains unmounted for maintenance. Do not bypass its
  dirty check or run applications against it before filesystem recovery.
- Source inspection found that the software watchdog thread was allocated
  but never enqueued. A candidate fixes that and makes its per-CPU accounting
  checks diagnostic only: it cannot detect a complete hard lockup while its
  own scheduler or timer cannot run. A private QEMU boot confirmed the monitor
  thread ran; independent hard-lockup detection remains future work.
- The maintenance initrd's Bash is dynamically linked into the unavailable
  `/nix`, and the RAM transfer helper creates non-executable files. A bootstrap
  that worked in a QEMU desktop with `/nix` is therefore not valid on the
  current Dell maintenance boot. A separate recovery initrd is being prepared
  with a fixed-path, read-only system-store snapshot helper; host and QEMU
  transfer tests passed. The production helper also passed a full 3.75 GiB
  snapshot from an unmounted store on a private QEMU SATA system volume, with
  independent host readback (`system-store-snapshot-qemu-v2-20261031`). This
  does not establish physical SSD snapshot or repair behavior. No physical
  recovery write has been attempted.
- The recovery USB was subsequently staged with the tested standalone
  snapshot helper as generation 24 in slot B. Before staging, both prior
  records and pairs were verified; generation 23 in slot A and the root
  recovery pair were left untouched. The old slot B and both selection records
  were saved and verified on host storage. The new slot and selection were
  flushed and read back, then the USB was unmounted. A physical recovery boot
  and actual SSD-store snapshot are still pending.
- Later, the Dell booted the recovery USB and the unmounted store was read
  remotely. The user chose direct repair instead of completing the slow image
  copy, accepting reinstall if needed. A standalone static `e2fsck` staged in
  recovery RAM found inode and bitmap inconsistencies; automatic preen returned
  1 (errors corrected), and a complete subsequent read-only check returned 0.
  The next checked restart selected the SSD payload and reported the persistent
  desktop store ready. The original Firefox-associated hang remains unresolved;
  this recovery does not prove that future crashes are handled automatically.
- A subsequent clean power-off and SSD boot with the USB stick removed again
  admitted the persistent store, but xHCI reset failed and no Ethernet address
  was acquired. The user observed repeated reset failures and reported that
  the Dell firmware's F12 menu also did not list either of two USB sticks.
  That firmware observation has not yet been explained; it may be independent
  of Brook's reset path. A controller reset candidate adds Intel's post-reset
  quiet interval, bounded phase checks and on-screen phase/register diagnostics.
  Host mocks and QEMU tests do not establish its behavior on the physical Dell.
  A full power disconnection subsequently restored USB detection in both
  firmware and Brook. On USB recovery boot, `e2fsck -f -n` found no filesystem
  inconsistencies, `e2fsck -p` cleared the unclean state, and a second
  read-only pass succeeded. The exact candidate recovery kernel and initrd
  passed a QEMU USB/network roundtrip; the physical-device-bound initrd
  correctly refused QEMU's different SSD identity. USB generation 25 was then
  staged while retaining the previous generation and root recovery.

## Installation plan and historical engineering gates

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
