# Batch TRIM for the Dell SSD

Brook can plan or run ATA TRIM during a guarded maintenance boot. Its default
boot performs no scan. TRIM tells the SSD that it can reclaim blocks from
deleted files, which can reduce write amplification. The effect depends on
workload and drive firmware.

## Request

A maintenance pair carries `/etc/ssd-trim` on the boot initrd. Use a separate
temporary payload for a single maintenance boot. The marker accepts these
forms, with a trailing newline. An empty marker selects a dry run.

| Marker | Operation |
| --- | --- |
| `dry-run` | Plan extents of at least 1 MiB |
| `run` | Submit all planned extents |
| `dry-run 1 16` | Preview a sample of up to 16 MiB |
| `run 1 16` | Submit that bounded sample |

The numbers are the minimum run and maximum total in MiB. A maximum of zero
selects the whole partition. The console emits `SSD_TRIM_RANGE` JSON records
during a dry run and an `SSD_TRIM` JSON summary with ranges, candidate bytes,
planned or completed commands, duration and error. On failure, candidate
bytes may include ranges whose commands did not complete. Successful dry
runs report zero submitted commands.

## Safety

The guarded path checks GPT and filesystem identities and retains the data
partition write lease. The data volume mounts at `/nix`. The separate
`/media/system` FAT volume contains the loader and recovery assets and is
outside the discard window along with both GPT copies.

Brook checks the guard records and clean ext2 header before TRIM, then
validates the full filesystem in a first pass. The verifier accepts the
existing Dell geometry with 4 KiB blocks and classic sparse superblocks.
It derives the primary and backup superblock and descriptor ranges, block
and inode bitmaps, and inode tables independently of the free bits. Both
per-group counts and the superblock total must reconcile. A second pass
revalidates each group before emitting free ranges. The desktop allocator
remains unpublished throughout, so there are no deferred bitmap writes or
concurrent allocations. Other device I/O can take the AHCI lock between
commands.

Before the first discard, Brook writes, flushes and rereads the ext2 dirty
marker. It restores the clean marker only after every command completes.
A failure leaves the allocator unpublished, and a boot after an interrupted
discard follows the ordinary dirty-volume recovery path. The successful
desktop mount subsequently marks the volume dirty for its normal session.

The AHCI driver requires IDENTIFY word 169 bit 0. It respects a reported
word 105 limit and uses one 512-byte payload block when the limit is
unspecified. Each non-queued DATA SET MANAGEMENT command uses feature
TRIM, and each entry holds a 48-bit LBA and at most 65,535 sectors.
Unused entries are zero padded. Verification relies on preservation of
live data and discard accounting, independent of reads from discarded LBAs.

## Verification and rollout

The host tests cover IDENTIFY, ATA encoding, split and padded payloads,
range and metadata protection, count reconciliation and dirty refusal.
The disposable QEMU path is exercised via
`scripts/check_dell_image_qemu.py --ssd-trim dry-run` and `--ssd-trim run`.
These run alongside the ordinary pair check. The fixture deletes a seeded
file, verifies live file hashes, measures qcow2 allocation, and checks GPT,
FAT and ext2 after a clean shutdown.

The `dell-ssd-trim-v3` pair built from `e41edd69` passed the normal
guarded recovery-to-desktop check. Both TRIM modes passed on disposable
QEMU/KVM images using that pair's kernel. They found 62,607,360 candidate
bytes. Dry-run issued zero commands and left 70,975,488 bytes mapped.
The explicit run completed one command and reduced mapped data to
58,589,184 bytes, a reduction of 12,386,304 bytes (11.8 MiB). Ten live
file hashes matched in each fixture. GPT checks, FAT fsck and offline
ext2 fsck passed after checked shutdown. A host concurrency fixture
also verified that the boot lease refuses ordinary writes and writes
from the future desktop adapter while the range emitter is outstanding.

Receipts are in the worktree under `build/dell/pair-trim-v3/qemu-check`,
`build/dell/trim-dry-v3` and `build/dell/trim-run-v3`.

For a future physical rollout, first confirm the Dell's IDENTIFY support.
Prepare a temporary dry-run pair and review its absolute sector ranges
against the live partition table. Boot a separate bounded run only after
review, then compare live file hashes and run offline `e2fsck -fn` before
considering the full run. Keep the normal pair as the default. Periodic
explicit maintenance boots are preferable to running a full scan during
shutdown.
