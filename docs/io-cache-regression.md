# Mixed append cache regression

The prepared bootstrap built Brook and passed its native tests, but offline
filesystem checking found ten allocated, unreferenced blocks. A smaller
reproducer writes 64 KiB through the batched append path, appends 40 KiB through
the legacy small-write path, then deletes the file before cache eviction.
The old kernel leaves exactly ten 4 KiB blocks allocated.

Legacy indirect-pointer writes updated the pointer cache but left the full-block
cache stale. Reclamation read the stale image and missed the appended blocks.
Alternating batched and small appends could also reuse that stale image. The
fix invalidates the full-block image before each legacy pointer write and
updates or invalidates resident pointer-cache entries according to its result.
It preserves the existing allocation/error policy; it does not add journaling
or recovery from partial device writes.

The I/O acceptance test now runs mixed append/unlink, append/truncate and
large/small/large append checks before its larger working set can hide stale
metadata by eviction. Run it inside the project's Nix shell:

```sh
scripts/run_io_benchmark.sh --output build/io-check-new \
  --backend ram --cache-probe --lifetime-probe --wait-mask-probe
```

Validation on 2026-10-15:

- The old Release kernel failed offline `e2fsck -fn` with exactly ten leaked
  blocks (`build/io-bench/mixed-old-repro`). No repair was performed.
- The fixed kernel passed all three mixed-append cases with 1, 2 and 4 KiB
  ext2 blocks, plus concurrent read/mmap, inode lifetime and signal/readiness
  checks. Both disk images passed offline checking after each boot, and cleanup
  recovered the exact initial free-block and inode counts.
- Results: `build/io-bench/mixed-fixed-all`, `mixed-fixed-1k` and
  `mixed-fixed-2k`. The first run overlapped a CPU-heavy sanitizer test; its
  elapsed time is unsuitable as a performance baseline.

The prepared bootstrap still needs a fresh acceptance run. Its earlier
successful compilation does not compensate for the failed filesystem check.
