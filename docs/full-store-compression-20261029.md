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
