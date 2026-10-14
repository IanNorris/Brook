# Brook bootstrap toolchain

This exports prebuilt Linux tools for a source build running inside Brook. It
uses the repository's Nixpkgs pin and Clang/LLD 18, matching `shell.nix`.
The compiler runs in the guest; exporting its binary closure on the host is
not a guest source build or a bootstrap of Nix itself.

Host preparation:

```sh
python3 scripts/export_guest_package.py --expression tools/bootstrap-pkg \
  --name brook-bootstrap --version 1
python3 -m http.server 18474 --bind 127.0.0.1 --directory build/package-cache
```

In a fresh guest, run these commands one at a time and wait for installation
completion before proceeding. The host cache is needed only for installation.

```sh
run /nix/profile/bin/bash -c "curl -fsS http://10.0.2.2:18474/brook-bootstrap.idx >> /nix/index/packages.idx"
run /nix/profile/bin/env NIX_CACHE_URL=http://10.0.2.2:18474 /nix/bin/nix-install brook-bootstrap
run /nix/profile/bin/brook-bootstrap-shell
```

The shell supplies the unwrapped cross compiler, hosted test compiler, Python
with freetype/Pillow, Git, CMake, Ninja, mtools, Hack font, utilities and TLS certificates.
Mtools is required by the kernel's generated FAT test image; older SDK
checkpoints can install it directly with `run /nix/bin/nix-install mtools`.
Inside that shell:

```sh
mkdir -p /data/bootstrap
cd /data/bootstrap
git clone --depth 1 https://github.com/IanNorris/Brook.git Brook
cd Brook
git submodule update --init --depth 1 -- vendor/uefi-headers src/third_party/fatfs
export CMAKE_BUILD_PARALLEL_LEVEL=1
./scripts/build.sh Release
ctest --test-dir build/release/host_tests --output-on-failure
```

A core bootstrap requires newly generated `build/release/kernel/BROOK.elf`,
`build/release/bootloader/BOOTX64.efi`, driver modules and bare-metal test ELFs.
Running host tests in Brook additionally exercises its native process/thread
compatibility. Booting the guest-built kernel is a further verification step.
All runtime images, package NARs, guest clones and logs stay outside git.

The bootstrap investigation also reproduced a cross-mount mmap cache collision:
a Git object on `/data` and Python's `_ctypes` library on `/nix` both had inode
3365. Before the fix, the library's mmap differed from `pread` at 1,736 bytes,
including its ELF header. Mount-specific cache identities restored matching
bytes, passed the equal-inode regression in `tools/at-syscall-probe`, and restored
`import ctypes`. Git subsequently updating a source file exposed stale cached
bytes on the same mount, including six null bytes past the new EOF. Writable
ext2 regular files therefore bypass shared mmap caching until mutation
synchronization is implemented. The append/pwrite/O_TRUNC/inode-reuse probe
failed four cases before this bypass and passed all cases afterward. This
retains demand paging and ext2 block caching, but loses cross-process page
sharing and preload read-ahead; restoring them is a performance follow-up.
