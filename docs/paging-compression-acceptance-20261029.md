# Bounded paging/compression acceptance — 29 October 2026

Branch: `wip/paging-compression-20261028`. This is an opt-in experimental pager,
a bounded compressed-RAM tier and read-only indexed immutable filesystem; it
does not enable general anonymous paging for shared application address spaces.

## Delivered and verified

- Dedicated plain page-to-disk store, generation/ref ownership, reserved
  fault-in frames, bounded reclaim, acknowledged TLB retirement, cancellation
  and mapping revalidation. Failure/short/corrupt I/O, stale publication,
  fork/CoW, partial unmap/replacement, protection changes, discard, resizing,
  leases and teardown have hosted and native coverage. The final recorded
  native matrix has 24 plain, 21 compressed and six executable-admission jobs:
  all 51 have collected exit status 0 and zero live tasks.
- Bounded LZ4 compressed RAM, incompressible bypass and disk spill, with all
  pools/slots returning to zero after stress. A 512-MiB guest can process a
  256-MiB pageable target where the eager control runs out of memory at 96 MiB.
  Under the fixed synthetic compressible workload, scan/fill are about 2.8×
  faster than plain disk paging; this is not a browser-memory speed claim.
- Versioned indexed 64-KiB compressed immutable blocks, checked host
  build/list/inspect/verify/extract/tar-export tools, safe full ext2 export and
  source metadata/hardlink preservation. The complete installed Nix store is
  11.98 GB raw versus 6.03 GB LZ4 (49.7% smaller). The resident serialized index
  is 21.12 MiB with a 32-MiB safety ceiling; file-cache residency is separate
  and reclaimable. User data remains ordinary ext2.
- Ninety measured host codec/corpus/block-size combinations compare LZ4,
  LZ4HC, Zstd and LZAV, with roundtrips, frozen inputs, memcpy control and
  separately checked freestanding scalar LZ4. LZ4 remains the measured fit;
  none is literally memcpy speed on the tested nonzero executable data.
- Four full-store low-memory controls verify every one of 92,590 files,
  11,959,287,571 bytes and digest `e4712804ff3db47b`. Sequential read is
  75.37 → 60.18 s (20.2% reduction); whole-file mmap is 121.30 → 89.32 s
  (26.4%). All controls complete checked shutdown and clean offline fsck.
- The matched animated-client pressure gate passes at 1 GiB RAM: loaded
  present-return p95 is 40.44 ms raw and 39.40 ms LZ4; p99 and worst interval
  also meet the predeclared limits. Complete traces cover the workload and
  Terra verifies visible animation. This measures presentation-call pacing,
  not physical scanout or input latency. Eighteen filesystem checks across
  the four pressure and two animated controls all pass.
- Fresh repaired-kernel raw/LZ4 Steam controls each pass 64 concurrent native
  TCP close/reuse sessions, authenticated Store visual review, normal Steam
  exit, checked guest sync/shutdown, QEMU owner exit 0 and clean data/Nix/tmp
  fsck. An exposed pre-existing socket lock reset race is fixed, with actual
  net.cpp lifecycle tests repeated 100 times, address/UB sanitizers and
  compiled entry/scheduler mutation audits. No forced stop is counted as a
  clean acceptance pass.

## Practical limits and evidence

Host caches were not globally flushed; these are controlled guest workload
observations, not physical cold-SSD results. Steam startup and desktop boot
show no established repeatable improvement. Writable compression, compressed
boot payloads and general shared-AS paging are future work. FIFO remains the
reclaim default; the optional clock experiment has no proven workload win.

Detailed scope, flags, invariants, source references and chronological results
are in `docs/paging-compression.md`, `docs/full-store-compression-20261029.md`
and `docs/socket-slot-lifetime-20261029.md`. Private authoritative receipts,
traces, image/ELF hashes and preserved failures are under
`build/paging-compression-20261028/` and
`build/reviews/paging-steam-20261029/`. Images, proprietary assets, account
material and unredacted screenshots are excluded from Git.
