# Signed boot payload compression

The shared UEFI loader supports signed LZ4 kernel and initrd files. Existing
raw ELF and FAT files still boot. Guarded slots and stick sets verify their
stored file hashes before checking the compressed envelope. The envelope's
Ed25519 signature authenticates the role, lengths and SHA-256 of the LZ4
bytes. Its message starts with `BROOK-BOOT-COMPRESSION-V1` and a NUL byte,
followed by the 64-byte envelope header. The loader verifies those fields
before allocating the output,
then requires the decoder to produce exactly the authenticated size.

The builder option is off by default. Use a dedicated 32-byte Ed25519 seed in
a private file at mode 0600. Never reuse an operator, profile-signing, stick
update, or any other signing key. Test loaders and fixtures need their own
disposable seed. Rebuild any prototypes signed without the domain prefix.

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

Compressed sets are for loader-aware offline assembly and QEMU. The transfer
receivers accept only raw incoming pairs. The RTX receiver refuses to stage
an update if the running root or PREV set is compressed. An incoming loader
could lack the public key needed to boot the preserved fallback. Update such
a stick offline using a matched loader and set.

The Dell system receiver can upload a raw pair into the inactive slot while
running from a compressed slot selected in VERIFIED mode. It requires the
loader's selector digest and rechecks both stored hashes before writing.
An unverified compressed pair is ineligible for remote promotion or fallback
inference. The ordinary raw Dell pair remains at the root of the build
output.

The first eight bytes `BRKCMP01` are reserved. A raw `INITRD.IMG` beginning
with those bytes is treated as a signed envelope and boot fails if validation
fails.

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
