# Fast I/O stability and performance gate

From a built checkout, inside `nix-shell`:

```sh
scripts/run_io_benchmark.sh --output build/io-bench/my-run --lifetime-probe
```

The default uses **eight vCPUs, eight concurrent workers, 2 GiB guest memory**,
private host tmpfs images under `/dev/shm`, and disabled audio. The experiment
and bootstrap launchers also default to eight vCPUs; explicit overrides remain
available. Build a matching kernel and modules first with `scripts/build.sh Release`.

Two workload shapes stand in for the expensive bootstrap paths: large-file
extraction writes and small-file checkout/installation operations. This is a
focused filesystem gate, not a substitute for a complete prepared-image or
clean-Nix bootstrap.

Each worker writes an 8 MiB file in 64 KiB chunks, crossing both direct and
single-indirect boundaries into double-indirect blocks. Every byte is checked
against a worker- and block-specific pattern. Other cases patch mapping
boundaries, append, rewrite an odd-sized 283,688-byte file, interleave fragmented
files, verify a 65,537-byte tail, grow an unlinked open file through double-indirect
blocks, and perform 64 small-file cycles per worker. Large unaligned reads
check both contents and adjacent guard bytes. The namespace workload
includes rename over an open victim and unlink while open. Its `operations`
field counts file cycles, rather than individual syscalls.

The harness synchronizes and shuts down, then requires read-only offline
`e2fsck -fn` to pass with the files still present. A second boot checks all
retained contents, removes the fixtures, and requires both another clean fsck
and exactly restored free-block/free-inode counts. The boot filesystem also
passes read-only `fsck.fat -n` after each shutdown. `--lifetime-probe` additionally
runs the existing 24 inode-lifetime assertions, including actual inode reuse.
Device errors, controller timeouts, missing completion markers and integrity
failures fail the run. No filesystem repair runs automatically.

`--thread-page-probe` additionally keeps a worker's anonymous, mprotect,
private-file and COW mappings alive after it exits, then requires a separate
process to reuse that worker's ID without corrupting any retained bytes. All
runs reject mapped-page leak/underflow diagnostics. See the bootstrap
[ownership diagnosis](bootstrap.md#worker-page-ownership-diagnosis-2026-10-15)
and `tools/at-syscall-probe/threadpagecheck.c` for the reproducer.

`--exit-group-probe` adds 128 rounds of concurrent futex, pipe and timer workers
racing normal joins and group exit. An explicit count, such as
`--exit-group-probe 512`, selects a longer run. Every child must become
waitable within its watchdog, and the guest process count must return to its
pre-probe baseline. Exit-group quiescence-timeout warnings fail the gate.

JSON records separate work and sync times for each phase, boot durations,
kernel identity, host CPU/load and memory/swap counters. Raw serial logs include
`/proc/blkprobe` and disk statistics before and after phases. Images are isolated
from user home, media and experiment disks. Successful images are deleted;
failed images remain at the location in `failure.json` for diagnosis. Only
small reports/logs are written to the checkout for RAM runs, and all generated
artifacts remain ignored under `build/`.

Tmpfs can be swapped by Linux; this reduces ordinary SSD writes but does not
promise zero SSD activity. The harness checks available memory and records
system-wide swap counters. Default test images consume considerably less than
their sparse logical capacity; the guest has a fresh 2 GiB data filesystem.
Both backends reserve the guest's configured capacity plus
`--host-headroom-gib` (default 4 GiB); RAM runs add a 1 GiB image allowance.
The check uses available host memory, not free memory alone. It cannot predict
unrelated applications growing afterward. Run one test VM at a time on a busy
desktop. Failed RAM images remain allocated until they are safely archived or
removed after the VM stops; they are not ordinary reclaimable file cache.

Use the same test against disk-backed images with:

```sh
scripts/run_io_benchmark.sh --output build/io-bench/ssd-run --backend disk
```

For larger workloads, use `--mib 32 --files 128`. Keep comparisons at equal CPU,
worker and workload settings. `--artifacts` selects a separate matching kernel,
EFI loader and modules directory. Performance results are informational;
stability checks are mandatory. Shared-host scheduling can materially affect
individual phase times, so compare repeated runs before setting thresholds.

## Initial measurements (2026-10-15)

The reviewed eight-worker RAM gate passes on main's baseline kernel and on the
experimental batching kernel, including offline fsck with retained files and
exact allocation restoration. The candidate additionally passed 24 lifetime
assertions. The baseline full gate took 21.72 seconds; the candidate took 22.89
seconds with the extra lifetime probe. These include image preparation and two
boots. Representative work-plus-sync times:

| Phase | Main baseline | Batching candidate |
| --- | ---: | ---: |
| 64 MiB sequential write | 1.067 s | 0.179 s |
| 64 MiB sequential read | 0.947 s | 0.905 s |
| Boundaries and fragmented overwrite | 0.348 s | 0.847 s |
| 512 small-file cycles | 0.590 s | 1.870 s |

The candidate reduces sequential-write requests from approximately 45,000 to
4,500 and improves that phase consistently, but regresses other phases. It is
preserved on `experiment/ext2-indirect-batching-20261015`, **not merged into
main**. Small-file wait totals/request counts are similar; the extra elapsed
time needs CPU/scheduling profiling before promotion. A disk-backed run of an
earlier probe version passed in 114 seconds, with small files dominating; that
run also overlapped host test activity and is not a controlled final comparison.
Reviewed RAM runs recorded no increase in the system-wide swap-out counter.
Evidence remains ignored under `build/io-bench/`.

The final shipping-main run, including the optional 24-check lifetime probe,
passed in **22.18 seconds**, restored both allocation counters exactly, and
recorded no additional swap-out pages (`main-final-ram-8cpu`). Native validation
passed 13/13 and bridge contracts passed 26/26. Syntax/diff checks also passed.

## Concurrent page-cache regression

```sh
scripts/run_io_benchmark.sh --output build/io-bench/cache-run \
  --cache-probe --lifetime-probe
```

The optional cache probe forks eight processes over the retained 64 MiB working
set. It checks 16,384 randomly selected 4 KiB pages through both `pread` and
private `mmap`, covering a set larger than the virtio and ext2 caches. This
models concurrent compiler/shared-library demand paging, which the sequential
read test alone did not cover. Its guest elapsed time is saved separately.

An eight-job prepared bootstrap exposed Clang instruction pages containing
zeros. Code review found an unlocked virtio cache-hit path copying a slot while
another CPU could replace it. Cache tags and bytes now use the existing request
guard for both fills and reads. The small-read path also limits the number of
covered blocks: an unaligned 64 KiB read spans 17 blocks and cannot use the
16-slot batch pool. The first fixed-kernel run passed the concurrent probe,
24 lifetime assertions, reboot persistence, offline fsck and allocation
restoration in 25.37 seconds. Full bootstrap verification is a separate gate.

## Refined storage candidate (2026-10-15)

The refined append path stages indirect pointers and commits the inode once per
64 KiB logical chunk. Small writes and overwrites keep their original path.
Ext2 cache buffers now use owned physical pages for 1–4 KiB filesystem blocks,
keeping thousands of permanent buffers out of the first-fit heap. This removes
the small-file regression observed with append batching alone; unmount releases
both data and indirect cache storage. Smaller filesystem blocks still reserve
one 4 KiB page per occupied slot.

The expanded eight-worker RAM gate passed with exact allocation restoration,
reboot persistence, concurrent pread/mmap checks, 24 lifetime assertions and
clean offline filesystems. Representative measurements from matching private
builds (before the subsequent scheduler and signal-wait changes):

| Phase | Cache-serialized baseline | Batched append + page-backed cache |
| --- | ---: | ---: |
| 64 MiB sequential write | 1.411 s / 45 MiB/s | 0.181 s / 354 MiB/s |
| 64 MiB sequential read | 1.105 s / 58 MiB/s | 0.132 s / 484 MiB/s |
| Boundaries, fragmented writes and open-orphan growth | 1.871 s | 0.612 s |
| 512 small-file cycles | 0.696 s | 0.579 s |

The optimized complete run took 20.12 seconds including two boots. Separate
1 KiB and 2 KiB filesystem gates passed after fixing the scatter-gather read
fallback: unmapped destination pages must never become DMA address zero, and
byte-granular requests must not DMA beyond their requested buffer. The new
unaligned-read sentinel test covers the latter boundary. These runs recorded
system-wide swap activity; RAM backing does not establish zero SSD writes.
Evidence is under ignored `build/io-bench/append-cache-pages*`.

`--block-size 1024|2048|4096` selects the ext2 fixture block size.
`--profile-small` captures small-file CPU/context-switch activity; its phase
timings are instrumented. `--wait-mask-probe` adds temporary-mask, child-signal
and select-readiness regression probes after the filesystem workload. These
optional probes support bootstrap diagnosis; the normal fast gate stays small.

## Checked device flushing (2026-10-15)

Module ABI 6 negotiates virtio FLUSH and propagates filesystem/device errors
through fsync, fdatasync, synchronous writes, sync, unmount and the bridge.
Rebuild the kernel and all modules together. GPT forwards flushes to its parent;
USB storage uses SYNCHRONIZE CACHE. ext4 conservatively flushes each adapter
write to preserve journal ordering. RAM disks have nothing pending to flush.

Without negotiated FLUSH, QEMU disables backend write caching for the device
and forces stable completion for each write. The comparison below uses the
same source, eight vCPUs, eight workers and private disk images. The control
changes only `BROOK_VIRTIO_BLK_FORCE_WRITETHROUGH`; both versions execute the
checked sync paths. Times include each phase's sync:

| Phase | Write-through control | Checked write caching |
| --- | ---: | ---: |
| 64 MiB sequential write | 6.788 s (9.43 MiB/s) | 0.305 s (209.55 MiB/s) |
| Boundaries and fragmentation | 19.903 s | 0.832 s |
| 512 small-file cycles | 23.073 s | 0.497 s |
| Cleanup | 3.582 s | 0.208 s |
| Complete gate, including two boots | 90.27 s | 21.89 s |

These are representative shared-host runs, not isolated hardware benchmarks.
Both passed byte checks, offline filesystem checks and exact allocation
recovery. A separate disk run with injected flush failures passed in 24.41 s;
its write phase took 0.331 s. Evidence lives under ignored
`build/io-bench/flush-wt-disk-control`, `flush-disk-positive` and
`flush-disk-injected-r2`.

`--flush-probe` exercises successful fsync/fdatasync/O_SYNC/O_DSYNC, injects a
FLUSH failure immediately before fsync, requires EIO and a successful retry,
then injects another failure and requires shutdown to abort before QMP while
the VM remains running. A checked sync retry must succeed. The next boot
verifies every fixture byte before removing it. Both ext2 and FAT passed this
sequence in `build/io-bench/flush-fat-ext2-injected-r3`, followed by clean FAT
and ext2 checks and exact ext2 allocation recovery. Seven bare-metal tests, all 13 host
tests and the bridge/injector tests also pass. The private host injector checks
session ownership and exact ELF identity, and uses a temporary Unix GDB socket;
it is not an exposed MCP tool. Successful QEMU shutdown alone is insufficient
evidence because host shutdown can mask a missing guest flush.

FLUSH-only failures remain retryable. Failed or short ext2 writes retain EIO
for the mount lifetime, and FAT disk writes retain EIO until drive rebind:
legacy metadata paths cannot prove that a failed pointer, inode or secondary
FAT write was repaired merely because a later flush succeeded. These latches
are conservative error reporting, not journaling or automatic repair.

Mounted filesystem modules must remain loaded. The current void module-exit
API cannot honor checked-unmount failure and then safely release module code;
see the mounted-module unload item in [backlog.md](backlog.md). This limitation
is separate from normal checked VFS unmount and bridge synchronization.
