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

- Use the guarded direct ext2 mount at `/media/system`. Derive the sector
  window from the admitted GPT identity. Build exclusions independently of
  allocation bits, including primary and backup filesystem metadata and all
  guard and boot reservations. Refuse geometry or feature combinations whose
  metadata layout the validator cannot prove.
- Under ext2 allocation exclusion, sync and flush queued writes, reread
  metadata, and verify group and superblock free counts and filesystem and
  guard state before issuing any DSM. Brook currently serializes allocation
  with `g_ext2Lock`. If that lock is released between batches, revalidate the
  current group's ranges under the lock before issuing them. Keep allocation
  excluded until the command retires. Define what happens to affected ranges
  after an uncertain completion before enabling the runtime path.
- Limit each command to the admitted target, preserve AHCI I/O serialization
  and panic handling, and stop on a command failure. Provide an explicit
  maintenance command with dry-run by default and JSON for ranges, bytes,
  commands, elapsed time and errors. No automatic TRIM in the first rollout.

## Validation and rollout

The host test covers bitmap intersections and minimum lengths, reserved-sector
and partition checks, IDENTIFY gating, FIS and range entry encoding, range
splitting, padding and prevalidation before emission. Allocation race and
runtime admission tests require the runtime integration above.

On a disposable QEMU AHCI image with `discard=unmap`, validate dry-run and
execution against a Dell-style GPT/ext2 image with deleted files. Compare all
live files before and after, measure qcow2 host allocation, and run
`e2fsck -fn` after clean shutdown. Then build and check the Dell pair in QEMU.

For a later approved physical rollout, confirm IDENTIFY support and review a
dry-run against the live partition table. Run a small bounded batch first and
check live file readback and offline fsck before a full run. If that passes,
prefer periodic maintenance while idle. Putting a full scan on the shutdown
path would add latency and compete with the final flush.
