# Signed boot payload compression

The shared UEFI loader supports signed LZ4 kernel and initrd files. Existing
raw ELF and FAT files still boot. Guarded slots and stick sets verify their
stored file hashes before checking the compressed envelope. The envelope's
Ed25519 signature authenticates the role, lengths and SHA-256 of the LZ4
bytes. The loader verifies all of those fields before allocating the output,
then requires the decoder to produce exactly the authenticated size.

The builder option is off by default. For an isolated test, provide a private
32-byte Ed25519 seed at mode 0600.

- `build_rtx_smoke_files.py --boot-compression-seed SEED` emits the full file
  set with a matching public key pinned in BOOTX64.EFI.
- `build_dell_image.py --boot-compression-seed SEED` emits an extra pair in
  `boot-compressed/` after verifying the ordinary pair. Use the matching
  `build_dell_loader.py --boot-compression-public-key HEX` and selector
  records hashed over the stored bytes.
- For Dell QEMU, pass the matching fixture loader as `--efi` and the test
  seed as `--boot-compression-seed` to `check_dell_image_qemu.py`. The fixture
  signs its compacted initrd after all QEMU-only edits, then boots it through
  slot A. Use a fixture-only signing key.

Compressed sets are for loader-aware offline assembly and QEMU. Existing
remote receivers check raw ELF and FAT headers and require raw pairs. The
ordinary Dell pair remains available at the root of the build output.

## Codec comparison

The OVMF benchmark used three headless QEMU/KVM boots with the same Dell
kernel and initrd for each codec. Times are medians in milliseconds. The
load time includes reading and decoding the file. The figures exclude signature verification
and selector hashing.

| File | Encoding | Stored bytes | Decode | Read + decode |
|---|---|---:|---:|---:|
| Kernel | Raw | 6,401,208 | 0 | 5.65 |
| Kernel | LZ4 | 3,498,329 | 4.46 | 6.09 |
| Kernel | zstd -3 | 2,504,763 | 20.30 | 21.59 |
| Initrd | Raw | 33,554,432 | 0 | 47.50 |
| Initrd | LZ4 | 12,086,865 | 70.55 | 74.34 |
| Initrd | zstd -3 | 8,214,356 | 76.29 | 79.81 |

LZ4 had the shortest compressed load time in this fixture and reuses the
vendored freestanding decoder. Raw loading remained faster on QEMU's fast
virtual disk, so the setting stays opt-in. The benchmark used the upstream
[LZ4 block format](https://github.com/lz4/lz4/blob/v1.10.0/doc/lz4_Block_format.md)
and the upstream [zstd single-file decoder](https://github.com/facebook/zstd/tree/v1.5.7/build/single_file_libs).
