# Full installed-store compression acceptance

The paging/compression branch now has a read-only path from a preserved ext2
Nix disk to a complete indexed immutable store. This is a workload acceptance
experiment, not general shared-address-space paging or a claim that Steam's
loading bottleneck has been solved.

## Source and format boundaries

`scripts/ext2_image_export.py` reads an unmounted, clean Linux-created ext2
regular-file image through libext2fs. It pins the source descriptor, opens the
library read-only, checks supported filesystem/inode features and records the
source identity before and after traversal/extraction. It rejects unsupported
inode kinds, extended attributes and extended modification-time fields rather
than silently changing their meaning. BRKPACK1 stores modification times at
whole-second precision. The exporter does not preserve access/change/creation
times in the packed image. Journaled, extent-based, 64-bit and inline-data
filesystems are outside this helper's supported input.

Extraction creates a new private tree without privileged chown. A separate
manifest retains source UID/GID, mode, whole-second mtime, inode identity,
symlink text and regular-file SHA-256. Regular-file hardlinks remain hardlinks;
symlinks are never traversed. Packing with `--source-metadata` requires the
complete exported namespace and verifies bytes, metadata and hardlink topology
against that manifest. Privileged mode bits remain stripped for the immutable
nosuid image. Existing outputs are never replaced.

The actual installed Nix store has 139,353 paths, 136,009 packed inodes,
139,352 directory entries, 258,316 independently readable blocks and
11,959,287,571 unique regular-file bytes. Its serialized index is 22,147,072
bytes (21.12 MiB), exceeding the former 16 MiB cap. The cap is now 32 MiB;
it bounds metadata, not file/image size, and the mount allocates the actual
index size. Additional node/link state costs 2,720,180 bytes and scratch buffers
131,072 bytes: about 23.84 MiB total before the small mount structure/allocator
overhead. File-page-cache residency is separate and reclaimable.

## Reproducible host workflow

Run in the project Nix shell; it supplies explicit libext2fs/LZ4 paths because
ctypes discovery through ldconfig does not reliably find Nix-store libraries.
Use a new output directory with enough free space for the export, final images
and one temporary payload per simultaneous builder.

```sh
cmake --build build/release/host_tests --target brook_image_checksum test_compressed_image test_compressed_vfs -j8
python3 scripts/ext2_image_export.py /path/to/stopped-nix.img --directory /store --extract /new/private/tree --output /new/private/metadata.json
export BROOK_IMAGE_CHECKSUM_LIBRARY="$PWD/build/release/host_tests/libbrook_image_checksum.so"
export TMPDIR=/new/private/tmp-host
mkdir -p "$TMPDIR"
python3 scripts/compressed_image.py build /new/private/tree /new/private/store-lz4.brk --source-metadata /new/private/metadata.json
python3 scripts/compressed_image.py build /new/private/tree /new/private/store-raw.brk --codec raw --source-metadata /new/private/metadata.json
build/release/host_tests/test_compressed_image --streaming /new/private/store-lz4.brk > /new/private/lz4-files.json
build/release/host_tests/test_compressed_image --streaming /new/private/store-raw.brk > /new/private/raw-files.json
cmp /new/private/lz4-files.json /new/private/raw-files.json
```

The optional shared library calls the same checksum implementation as the
kernel reader. Without its environment variable, the portable Python reference
remains available. Differential tests cover bytes, mutable buffers, empty and
block-boundary inputs. A one-MiB host sample measured 8.23 MiB/s for Python and
1,063.76 MiB/s for the native implementation, with identical output. These are
checksum microbenchmarks, not filesystem or guest throughput measurements.

Use the verifier's `--streaming` mode for large images. It retains bounded
index/node/scratch storage and reads all file bytes incrementally. Its older
single-image-argument diagnostic loads the whole image into memory and is
unsuitable for a multi-GiB store. FNV-1a checks detect corruption, not malicious
replacement; the source manifest additionally checks exported bytes with SHA-256.

## Current measured artifacts

The raw packed image is 11,981,434,880 bytes; LZ4 is 6,025,334,784 bytes, including
the identical-sized indexes. 235,180 of 258,316 blocks use LZ4; the rest use raw
storage when compression does not shrink a block. The LZ4 image is 49.71%
smaller than the raw packed control. Image identities differ because offsets,
codec choices and block tables differ; file content must compare identically.

The preliminary signed-in Steam run observed its main window after 95.63 s,
but it followed an inventory job and is not a matched cold control. Steam read
about 1,488 MiB from its data disk versus 163 MiB from Nix. The client/runtime
on the data disk therefore dominates these bytes; compressing only Nix cannot
be expected to remove most of the startup delay. Window-title observation is
also weaker than authenticated pixel readiness.

Private source disks, exports, signed-in profiles, screenshots and detailed
reports remain under ignored `build/reviews/paging-steam-20261029/`. No account
material or game data belongs in git. Raw/LZ4 controls use independent reflinks
of the same stopped source, the same matched kernel/modules, 8 vCPUs, 8 GiB,
Intel GPU/Venus and ordinary application mappings. The original Nix disk still
provides the profile and mutable state; a read-only virtio device overlays
`/nix/store`. Page-store experimental executable enrollment is not enabled for
Steam's shared address spaces.

Full actual-reader verification passes for both images: 92,590 regular files
and 11,959,287,571 bytes each, with byte-identical per-inode size/checksum reports.
Raw verification took 36.04 s and LZ4 30.31 s, with observed peak child-process
RSS below 50 MiB. This is a host sequential integrity check, not guest startup
or a cold-storage benchmark. The final small suites pass nine image/tool tests,
eight exporter tests, both affected C++ host tests, and compiled entry/startup
mutation audits. The Python-reference image suite also passes independently.

A first launch exposed a harness setup issue, not image corruption: QEMU assigns
PCI addresses to explicit `-device` entries before shorthand `-drive if=virtio`
devices, so the extra packed disk appeared as guest virtio0 rather than virtio5.
The private setup-failure VM was synced and stopped, and its evidence retained.
The corrected controls pin the packed device to PCI address 0x10 and verify the
actual `brookimg: ready` serial message before launching Steam. Host QMP block
names must not be assumed to equal Brook's PCI-enumerated device labels.

## First Steam pair

Both packed-store controls rendered the authenticated Steam Store. Terra
visually checked the saved completed-frame captures; neither showed a login,
QR, or error screen. Guest Python confirmed all 1,049 immediate store entries
and the same resolved Python executable used by the original profile.

The raw main-window title appeared after 87.79 s; LZ4 after 85.26 s. Startup
reads from the packed device were 221.95 versus 101.92 MiB (54.1% fewer bytes),
with 3,849 versus 1,873 requests (51.3% fewer). The mutable Steam data disk read
1,509.52 versus 1,510.23 MiB. Its nearly unchanged, much larger load explains
why halving package-device traffic barely changes the whole launch time.
These are one forward pair with unflushed host caches and network activity;
the 2.53 s elapsed difference does not establish a repeatable startup speedup.

Host launcher start to debug readiness was 11.53 versus 9.63 s, including host
setup and firmware; it is not the boot-logo-to-desktop interval. At the initial
OS stats sample, used memory was 362.49 MiB in both controls. The same index and
runtime metadata are resident regardless of payload codec. These ordinary
8-GiB application runs establish compatibility and I/O reduction, not paging
behavior under pressure or compressed boot-kernel/module support.

The raw controller encountered a host-only screenshot serialization error
after recording startup. Its original VM was retained, the already-written
frame metadata recovered, and the same run completed the normal Steam
`-shutdown` helper and checked guest sync/shutdown. QEMU exit zero is verified;
the outer raw controller exited with its earlier harness error, so this is not
an all-green controller result. The LZ4 controller includes the corrected
metadata-only serialization and completes end to end, including normal client
shutdown, guest sync/shutdown, QEMU exit zero and original launch-owner exit
zero. Both original signed-in source disks remain untouched.

Detailed per-device deltas, private frames, job receipts and shutdown evidence
are in `first-pair-comparison.json`, `matched-raw/` and `matched-lz4/` under the
ignored evidence directory. Remaining work includes low-memory full-store
exercise and broader application/shared-AS paging; this pair does not close
the overall paging/compression goal.

## Full-store low-memory acceptance

The benchmark manifest now permits up to 131,072 unique regular files, covering
the full installed store rather than a reduced closure. Its generator computes
checksums incrementally with bounded 64-KiB reads. The optional native checksum
library exports an incremental FNV update; differential tests cover arbitrary
chunk boundaries, empty updates and invalid seeds. The image/tool suite passes
11 tests with native acceleration and independently with the Python reference.

Four fresh eight-vCPU private native-test VMs use the same frozen boot seed,
current kernel/module ABI and full raw/LZ4 stores. Sequential reads use 512 MiB
RAM and a 192 MiB ordinary anonymous allocation, leaving about 32 MiB free.
Whole-file mmap uses 1 GiB RAM with the same allocation: the largest individual
file is 477.5 MiB and must fit while actively mapped. This checks full-file
mapping behavior; it does not assume mapped clean pages can already be evicted.
Each pass verifies every file against the trusted manifest, checks the ordinary
allocation afterward, and completes original launch-owner exit zero following
checked sync/shutdown and QEMU exit zero.

Results from one forward raw/LZ4 pair per access mode:

- Sequential reads: 75.37 s raw versus 60.18 s LZ4, a 20.2% elapsed reduction.
  Packed-device reads are 11,928.26 versus 5,817.77 MiB. Minimum sampled free
  memory is 32.28 versus 32.54 MiB. These direct reads use bounded decoder
  buffers and do not populate the mmap page cache.
- Whole-file mappings: 121.30 s raw versus 89.32 s LZ4, a 26.4% elapsed
  reduction. Packed-device reads are 18,485.23 versus 6,622.23 MiB. The cache
  worker reclaims 81,408 versus 76,544 pages (318 versus 299 MiB). Each ends
  with the same bounded 65,536-page/256-MiB cache; compression does not shrink
  its decompressed resident pages.

Every pass verifies 92,590 unique regular files and 11,959,287,571 bytes, with
the same combined digest `e4712804ff3db47b`. The same-size index, node/link state
and decoder buffers cost about 23.84 MiB per mount in either codec. Mapped I/O
includes rereads and preloading; it is not equal to unique file bytes. Host
caches are not globally flushed, so these are guest-cold workload observations,
not physical cold-SSD benchmarks or proof of a universal startup improvement.
The measurements establish a useful full-store throughput gain alongside
bounded low-memory operation; more repetitions would be needed to quantify
variance tightly.

The first read-only setup pass itself verified all bytes, but the host harness
incorrectly required mmap-cache reclamation from the direct-read path. That
control remains marked failed with its evidence preserved. The four reported
controls use the corrected criterion: direct reads must start below 64 MiB free;
mapped passes must actually reclaim cache pages. No failed control is counted
among the four completed controls.

Evidence lives in `pressure-store/` under the private review directory:
`results.json`, per-control job/stats/device/shutdown/owner receipts and the
frozen staging hashes. No packed images or source manifests are committed.

## Remaining bounded responsiveness and boot gates

Before measuring an active desktop under the same pressure workload, use the
existing instrumented cube from `tools/vulkan-cube-timing-pkg` and its offline
analyzer. The file benchmark now emits guest monotonic `start_ns`/`end_ns` so
load windows can be matched to cube timestamps. The four results above predate
those additional output fields; their timed intervals remain unchanged.

Predeclare the following engineering acceptance thresholds, before inspecting
any new active-client trace:

- Match GPU, resolution, VM memory and anonymous load between raw/LZ4 boots;
  leave experimental executable paging enrollment disabled.
- Retain at least 1,000 complete presentation intervals and 20 seconds wholly
  inside the load window. Require successful client/workload completion, zero
  missing/error/overflow trace rows and verified cleanup.
- Loaded present-return interval p95 must be at most 50 ms and twice idle p95;
  p99 at most 100 ms and maximum at most 500 ms.
- LZ4 loaded p95 must be no more than 20% worse than the matched raw control.
  Report useful-work duration, actual memory headroom and any stalls alongside
  the latency. A failing idle baseline must be diagnosed without relaxing the
  criterion after seeing results.

These measure client presentation-call pacing, not physical scanout or input
latency. Verify visible animation before/during loading through Terra. Keep an
uninstrumented observer-overhead control. The compositor's short 256-interval
stats window can explain outliers but cannot replace the complete client trace.

The Steam/boot follow-up also still needs matched repeated controls with both
host launch and actual guest first-desktop-presented endpoints. The earlier
host debug-readiness observations are not substitutes for desktop boot time.
Compressed boot kernels/modules and general shared-AS anonymous paging remain
future work; neither should be silently added as a new requirement for closing
the original bounded paging/compression goal.

All twelve read-only ext2 consistency checks of the pressure controls' data,
Nix and tmp partitions pass after shutdown. The checks export only stopped
private qcow2 views, retain reports and remove temporary partition copies after
successful checking. The final timed probe with load-window markers also builds
with static musl and `-Wall -Wextra -Werror`.

The original Steam pair's actual desktop endpoint can be recovered from the
existing `BOOT: desktop presented` serial event: 5,060 ms raw and 5,125 ms LZ4.
Those guest-compositor clock endpoints differ from the 11.53/9.63 s host start
through debug-readiness measurements. They show no guest boot-time improvement
in this pair. Exact records are in `first-pair-boot-endpoints.json`. Repeated
matched controls and the active-client pressure gate remain pending.

## Active-client pressure acceptance

The declared responsiveness gate passes for both codecs using explicit staged
Venus presentation (`MESA_VK_WSI_DEBUG=sw,noshm`), Intel UHD 630, eight vCPUs,
1 GiB RAM and identical ordinary-memory pressure: two independent 160 MiB
holders plus the read probe's 192 MiB allocation. All three allocations verify
their bytes before release. Experimental executable paging enrollment remains
disabled. The full-store read and the animated 1280×720 client run concurrently.

Both cube traces contain exactly 8,192 submitted/ready frames, zero errors and
zero overflow. Windowed analysis validates the entire capture before selecting
only intervals whose two presentation returns fall inside the probe's recorded
guest monotonic start/end. Each capture spans the complete load window and
contains more than 1,000 loaded intervals and 20 seconds. Terra independently
verifies a well-formed cube changing orientation in saved pairs before and
during loading; those still images are not used to establish continuous pacing.

Measured presentation-return intervals:

- Raw: idle p95 37.64 ms; loaded p95 40.44 ms, p99 45.66 ms, maximum 67.47 ms.
- LZ4: idle p95 37.97 ms; loaded p95 39.40 ms, p99 43.85 ms, maximum 95.21 ms.

Both pass p95 ≤50 ms and ≤2× idle, p99 ≤100 ms and maximum ≤500 ms. LZ4's loaded
p95 also passes the ≤1.2× raw comparison. These are instrumented application
presentation-call intervals, not physical scanout or input latency. Brief
completed-frame capture pauses are retained in the measured intervals.

The simultaneous full-store verification takes 137.36 s raw versus 100.80 s
LZ4, a 26.6% reduction in this pair, with identical file counts/bytes/digests.
At the timed I/O start the probes report 58.16 and 59.01 MiB free. The cache
worker reclaims 10,866 and 10,908 pages respectively (about 42.5 MiB), preserving
forward progress while the client animates. Do not compare these absolute load
times with the earlier desktop-free 512-MiB test as if the configurations were
identical.

Uninstrumented/traced/uninstrumented 600-frame controls precede each main run.
Whole-job raw durations are 16.02/17.30/14.28 s; LZ4 durations are
17.61/16.04/15.84 s. These include startup, teardown and host polling. Their
variation does not isolate a stable per-frame observer cost; it does establish
that timestamps are not assumed free. Both codec comparisons use the same
observer. Do not advertise these captures as uninstrumented cube throughput.

The first automatic-direct setup is excluded: Vulkan work and timestamps
complete, but Wayland's legacy PRIME import rejects Venus buffers and no WM
window appears. The same restriction is present on main and predates the paging
branch. Rebuilding userspace alone does not bypass that source-level import.
The staged compatibility probe is visibly correct in the same preserved VM;
the proper matched controls then use fresh source reflinks. The direct-path
issue is recorded in the backlog, without bypassing its lifetime protections.

Both final staged controllers complete normal client/workload exits, checked
guest sync/shutdown, QEMU exit zero and original launch-owner exit zero. All six
read-only ext2 checks of data/Nix/tmp pass afterward. Detailed traces, matched
request receipts, visual evidence, phase analyses, comparison and filesystem
reports are retained under `responsiveness/` in the ignored evidence directory.
