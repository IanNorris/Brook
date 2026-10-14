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
Native C++ linking includes explicit libc++ library/rpath flags, since exported
compiler wrappers do not inherit a Nix build shell's propagated flags.
Mtools is required by the kernel's generated FAT test image; older SDK
checkpoints can install it directly with `run /nix/bin/nix-install mtools`.
Inside that shell:

```sh
mkdir -p /data/home /data/tmp /data/bootstrap
cd /data/bootstrap
git clone --depth 1 https://github.com/IanNorris/Brook.git Brook
cd Brook
git submodule update --init --depth 1 -- vendor/uefi-headers src/third_party/fatfs
export CMAKE_BUILD_PARALLEL_LEVEL=1
./scripts/build.sh Release
ctest --test-dir build/release/host_tests --output-on-failure --no-tests=error
```

A core bootstrap requires newly generated `build/release/kernel/BROOK.elf`,
`build/release/bootloader/BOOTX64.efi`, driver modules and bare-metal test ELFs.
Running host tests in Brook additionally exercises its native process/thread
compatibility. Booting the guest-built kernel is a further verification step.
All runtime images, package NARs, guest clones and logs stay outside git.

## Verified milestone (2026-10-14)

Inside Brook, an HTTPS clone of revision `e4aa4c0` and its two required
submodules produced the kernel, EFI loader, 12 nonempty driver modules, seven
bare-metal test ELFs and all 13 hosted test executables. The module validator
checked all 12 modules against 96 exported symbols. The guest-built kernel,
loader and modules then booted a separate VM with the desktop, virtio GPU,
storage, networking and command bridge; Git ran under that kernel.

The 13 hosted tests pass on the host. In Brook, all 13 currently segfault
during shared-library startup (CTest exit 8), before their assertions run.
This is a successful core source-build and boot milestone; native test runtime
compatibility remains [backlogged](../../docs/backlog.md). The compiler and
SDK are prebuilt Linux binaries, rather than a compiler built inside Brook.

The startup failure was subsequently traced to boot-library fallback supplying
musl for missing Nix store `libc.so` paths during linking. Exact Nix path lookup
and rebuilding remove that mixed-libc failure: the first three CTest cases pass,
and the threaded mutex test passes all eight checks directly. A second fix
notifies CTest when the last thread makes an exited leader waitable; the full
suite now completes with 10/13 passing on one vCPU. Two concurrency assertions
and a descriptor-stress timeout remain; a full guest suite pass is still
outstanding. Existing binaries accidentally linked against musl must be
rebuilt; changing path lookup alone cannot repair their dependencies.

The local extracted outputs and SHA-256 manifest are retained under
`build/experiments/results/bootstrap-20261014/guest-build-e4aa4c0`.
For another verification, sync and stop the build VM before extracting
`build/release` from its data disk. Keep the output layout (`kernel`,
`bootloader`, `tests`) and use a separate copy of the prepared boot/data/Nix
disks. Copy the guest-built modules into that boot disk's `DRIVERS` directory
and its `kernel/ksym_addr_table.cpp` into `KSYM_ADDR.TXT`, as for a host build.
Launch with both `BROOK_BUILD_DIR` pointing to the extracted build and
`BROOK_APPS_RUN` pointing to the separate disk directory:

```sh
BROOK_BUILD_DIR="$PWD/build/experiments/results/bootstrap-20261014/guest-build-e4aa4c0" \
BROOK_APPS_RUN="$PWD/build/experiments/results/bootstrap-guest-boot-e4aa4c0" \
BROOK_AUDIODEV=none nix-shell --run './scripts/run_installed_apps.sh'
```

The bootstrap experiment disks include repaired interrupted-run metadata and
are diagnostic artifacts, not replacements for the clean app checkpoint.

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
