# Batch TRIM for the Dell SSD

Work in progress on `work/ssd-trim`, based on `work/integration32` at
`7268f1b6`. The first change is a host-tested planner. Brook cannot issue
TRIM through this change.

TRIM tells the Samsung PM851 which sectors no longer contain useful data.
This can reduce write amplification when the SSD reclaims space. The effect
depends on the workload and drive firmware. Reducing unnecessary log writes
remains useful independently.

## Command and range planning

Use ATA DATA SET MANAGEMENT with the TRIM feature bit, through the existing
non-queued AHCI command path. Require IDENTIFY word 169 bit 0 and a usable word
105 payload limit. Split long extents into entries of at most 65,535 sectors.
The planner encodes little-endian entries and zero-pads unused slots. A batch
must fit both the controller's DMA buffer and the drive's advertised limit.
Never use contents read from trimmed sectors to verify a command.

The pure planner prevalidates the entire range list before producing the first
payload. Ranges must be sorted, non-overlapping, inside the selected partition
and outside all supplied exclusions. The bitmap helper finds runs free in both
the on-disk and cached copies and skips runs shorter than the specified minimum.
For the Dell’s classic ext2 layout the group validator derives primary and
backup superblock and descriptor reservations, both bitmaps, and the inode
table. It checks that both bitmaps reserve these blocks and that each copy's
free count matches the group descriptor.

## Runtime integration still needed

- Take the ext2 sector window from the guard’s admitted GPT data reservation.
  The direct ext2 volume appears at `/nix`. The guarded `/media/system`
  mount is the EFI partition and must remain outside the discard window.
- Use the clean pre-mount window after guard admission and before publishing
  the ext2 allocator. The direct session intentionally marks the primary
  superblock dirty before mounting, so an online scan cannot satisfy the
  clean-header requirement. Keep the boot lease and maintenance ownership
  through both passes and command retirement. If completion is uncertain,
  leave the allocator unpublished and return to maintenance.
- The two-pass scan checks every group and the superblock free total before
  emitting any ranges. It derives primary and sparse backup reservations
  independently of the allocation bits. The guard must pin the partition
  and exclude writers across both passes. In this window no ext2 cache
  exists, so there are no deferred allocations or frees to reconcile.
- Limit each command to the admitted target, preserve AHCI I/O serialization
  and panic handling, and stop on a command failure. Provide an explicit
  maintenance command with dry-run by default and JSON for ranges, bytes,
  commands, elapsed time and errors. No automatic TRIM in the first rollout.

## Validation and rollout

The host test covers bitmap intersections and minimum lengths, reserved-sector
and partition checks, IDENTIFY gating, FIS and range entry encoding, range
splitting, padding and prevalidation before emission. A synthetic two-group scan also verifies that a late bitmap mismatch, an
incorrect superblock free total and a dirty header all suppress emission.
Writer-exclusion and runtime admission tests require the integration above.

On a disposable QEMU AHCI image with `discard=unmap`, validate dry-run and
execution against a Dell-style GPT/ext2 image with deleted files. Compare all
live files before and after, measure qcow2 host allocation, and run
`e2fsck -fn` after clean shutdown. Then build and check the Dell pair in QEMU.

For a later approved physical rollout, confirm IDENTIFY support and review a
dry-run against the live partition table. Run a small bounded batch first and
check live file readback and offline fsck before a full run. If that passes,
prefer periodic maintenance while idle. Putting a full scan on the shutdown
path would add latency and compete with the final flush.
