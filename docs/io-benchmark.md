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
files, and perform 64 small-file cycles per worker. The namespace workload
includes rename over an open victim and unlink while open. Its `operations`
field counts file cycles, rather than individual syscalls.

The harness synchronizes and shuts down, then requires read-only offline
`e2fsck -fn` to pass with the files still present. A second boot checks all
retained contents, removes the fixtures, and requires both another clean fsck
and exactly restored free-block/free-inode counts. `--lifetime-probe` additionally
runs the existing 24 inode-lifetime assertions, including actual inode reuse.
Device errors, controller timeouts, missing completion markers and integrity
failures fail the run. No filesystem repair runs automatically.

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
