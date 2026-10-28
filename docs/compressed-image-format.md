# BRKPACK1 immutable compressed image

Status: host tooling and shared C++ reader implemented and tested. **The VFS
mount adapter and native executable/Nix-closure acceptance are not implemented
yet.** This format is separate from disposable runtime swap. User data and
package installation remain on ordinary writable filesystems.

## Purpose and layout

Each regular file has independently indexed 64 KiB blocks, allowing bounded
random reads without decoding an entire file. A block is raw or LZ4; the builder
keeps raw bytes whenever compression does not shrink the block. The reader
requires exact decoded length and checks the original bytes before publishing
its one-block cache. Metadata and payload checksums are FNV-1a64 corruption
checks, not authentication or a secure-boot trust decision.

All integers are little endian. The authoritative shared declarations are
`src/shared/compressed_image_format.h`; Python uses matching explicit struct
formats. Fixed sections are contiguous in this order:

1. Header, 160 bytes.
2. Inodes, 64 bytes each.
3. Directory entries, 16 bytes each, sorted by parent inode and unsigned name bytes.
4. Block records, 32 bytes each.
5. String bytes, followed by zero padding to a 4096-byte boundary.
6. Contiguous payloads in block-record order, followed by padding to a
   4096-byte image length. Final payload padding carries no data semantics.

The header contains magic `BRKPACK1`, version 1, header size, required-feature
bits (zero in v1), image length, block size 65536, root inode zero, table offsets
and counts, string offset/length, payload offset, a nonzero 16-byte identity,
metadata checksum, header checksum, and sixteen reserved zero bytes. Metadata
checksum covers bytes from offset 160 through the payload boundary, including
zero padding. Header checksum covers bytes [0,136), including the metadata
checksum. Unknown features, unsupported versions and noncanonical table layouts
fail closed. Image length may be smaller than its backing device, but may not
exceed it. The complete serialized index including header/padding is capped at
16 MiB before allocating it.

An inode records kind (1 directory, 2 regular, 3 textual symlink), full Unix mode,
UID/GID, size, first block/count, symlink target offset/length, reserved zero word
and signed mtime seconds. Each file owns its exact contiguous block-record range
in inode order; empty files have no blocks. Directories have no data extent.
Symlink size equals target byte length; targets are literal strings and may be
absolute or contain `..`. Following them belongs to VFS, not the image reader.
Special files are unsupported. Directory records hold parent/child inode,
name offset, name length and a reserved zero halfword. Regular hardlinks reference
the same inode; directory hardlinks and root aliases are rejected.

Names are 1..255 non-NUL bytes, contain no slash and are neither `.` nor `..`.
Targets are 1..4095 non-NUL bytes. Directory parent chains must reach root in at
most 128 steps, every nonroot inode must be referenced, and paths including the
initial slash fit 4095 bytes. Duplicate names and unreachable/cyclic subtrees
are invalid. Names need not be UTF-8; host JSON uses escaped representations.

A block records absolute payload byte offset, stored length, decoded length,
codec (0 raw, 1 LZ4), reserved zero word, and checksum of decoded bytes. Both
lengths are 1..65536; raw lengths match and LZ4 is strictly smaller. The final
file block has its exact remaining byte count. Arbitrary payload byte alignment
is intentional: a device adapter must support exact unaligned reads. The
bounded index and caller-owned per-inode validation state reject overlapping,
unowned, overflowing and out-of-device extents before any payload read.

## Resident reader state

`fs/compressed_image` borrows immutable resident index bytes and a 16-byte
validation/reference record per inode. Its caller supplies independent 64 KiB
encoded and decoded buffers, a read callback, a safe decoder and serialization.
The 16 MiB index cap does not include this node state, buffers, mount/vnode
objects, page cache or allocator overhead. No read/decode allocation occurs.

A failed/short device read, malformed decode or checksum mismatch leaves the
cache invalid. No bytes from that block reach the caller. If a multi-block read
already copied verified bytes, it returns that prefix; the failing block remains
an error on retry. Adjacent 4 KiB reads can reuse one decoded 64 KiB block.
The index and backing device must remain immutable for this cache lifetime.

The VFS adapter must provide normal path and object-relative operations,
mount/inode identities without cross-filesystem cache collisions, explicit
EROFS mutation failures, and backing lifetime through retained objects/vnodes
and mapped files. It must not free index/cache state on pathname detach while
retained handles can still use it. Native acceptance requires mounted reads,
symlinks/hardlinks, cross-block reads, permission/mutation checks, normal ELF
execution and a representative Nix closure at its expected paths. These remain
outstanding, not implied by host reader tests.

## Host commands

Build the already-vendored codec on a Linux/Nix development host; no separately
installed LZ4 library is needed:

```sh
nix-shell --run 'scripts/build_image_codec.sh'
python3 scripts/compressed_image.py build SOURCE output.brk \
  --lz4-library build/host-tools/libbrook-image-lz4.so
python3 scripts/compressed_image.py inspect output.brk
python3 scripts/compressed_image.py list output.brk
python3 scripts/compressed_image.py verify output.brk \
  --lz4-library build/host-tools/libbrook-image-lz4.so
python3 scripts/compressed_image.py extract output.brk NEW_DIRECTORY \
  --lz4-library build/host-tools/libbrook-image-lz4.so
python3 scripts/compressed_image.py export output.brk output.tar \
  --lz4-library build/host-tools/libbrook-image-lz4.so
```

`build --codec raw` creates the same indexed layout without compressed blocks,
for correctness/performance controls. Raw-only inspect/list/verify/extract/export
need no codec library. The supplied host codec build helper also selects a
`.dylib` on Darwin, but that path is untested. Current codec tooling pins the
vendored LZ4 1.10.0. Host builds use ordinary user-space compiler/runtime rules;
kernel decode reuses the separately built freestanding scalar codec.

Packing streams file payloads through a temporary file instead of retaining
the complete data in RAM. It rejects source symlink traversal and source files
that change during their read, preserves regular hardlinks, and normalizes
timestamps to zero. It strips privileged mode bits. This is not an atomic
snapshot of a changing source filesystem. Images are created exclusively and
never overwrite an existing path. A failed build may leave an incomplete new
output, which cannot pass validation.

Extraction requires a newly created destination and uses directory-relative
no-follow operations and exclusive file creation. It never follows image-created
symlinks, including absolute/escaping targets; they are reproduced as text.
It preserves regular hardlinks, modes without privileged bits, and normalized
timestamps, and never restores ownership. Restrictive directory modes are
applied after population. The newly created output tree must not be concurrently
modified by another actor. A payload error fails the command and can leave an
explicit partial new tree; it does not merge into existing files. Export produces
a standard tar archive with relative entry names and the same literal links.

## Validation evidence

`test_compressed_image` compiles the actual C++ reader with freestanding LZ4.
It tests table/extent overflows, duplicate names, directory cycles/hardlinks,
mode and final-block mismatches, raw/LZ4 reads, block-cache hits, malformed
payload, short/error I/O, recovery and verified-prefix behavior. The same reader
is built into the kernel, but is not yet mounted. ASan and UBSan pass.

`scripts/test_compressed_image.py` performs deterministic build, byte/hash
cross-checks against that C++ reader, hardlink/symlink extraction and tar export,
no-overwrite/existing-destination checks, unsupported special-file rejection,
empty/raw images, maximum-depth/length boundaries, corruption/truncation,
orphan/cycle/directory-hardlink cases and eighty seeded metadata mutations with
valid recomputed checksums. Python and C++ accept/reject results agree. Run it
after building the reader and the host codec:

```sh
BROOK_IMAGE_TEST_LZ4_LIBRARY="$PWD/build/host-tools/libbrook-image-lz4.so" \
  python3 scripts/test_compressed_image.py
```

Evidence is retained in `build/paging-compression-20261028/compressed-image-*`.
Full filesystem, executable compatibility and performance claims await native
mount integration; this checkpoint establishes the format, tools and shared
reader, not the completion of compressed on-disk OS storage.
