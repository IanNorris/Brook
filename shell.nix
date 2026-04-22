# Brook OS development shell for NixOS / nix-shell
# Usage: nix-shell (from repo root)
#
# Two compilers are available:
#   CC / CXX   — unwrapped Clang for kernel cross-compilation (freestanding targets).
#   HOST_CC / HOST_CXX — wrapped Clang+libc++ for host-native builds (unit tests).
#
# mkShellNoCC is used to prevent Nix from injecting system include paths that
# break libc++'s #include_next header chain.

{ pkgs ? import <nixpkgs> {} }:

let
  llvm = pkgs.llvmPackages_18;

  # A properly wrapped Clang+libc++ toolchain for host-target builds.
  hostCC = llvm.libcxxStdenv.cc;

  # Musl cross-compiler for building static userspace binaries (DOOM, etc.).
  muslCC = pkgs.pkgsCross.musl64.stdenv.cc;
in
pkgs.mkShellNoCC {
  name = "brook-dev";

  packages = [
    # Kernel cross-compilation: unwrapped so cross-targets
    # (x86_64-unknown-windows, x86_64-elf) aren't polluted by Nix cc-wrapper flags.
    llvm.clang-unwrapped
    llvm.lld          # provides lld-link and ld.lld

    # Host-target compiler (wrapped Clang+libc++) for unit tests and tools.
    hostCC

    # Musl cross-compiler for static userspace binaries.
    muslCC

    pkgs.cmake
    pkgs.ninja

    # Runtime / testing
    pkgs.qemu
    pkgs.OVMF
    pkgs.vde2            # vde_switch + slirpvde for VM<->VM networking

    # Dev tools
    llvm.clang-tools      # clangd, clang-tidy, clang-format

    # Font baking (build-time TTF → kernel glyph atlas)
    pkgs.hack-font
    (pkgs.python3.withPackages (ps: [ ps.freetype-py ]))

    # Storage image creation (build-time FAT32 ramdisk image for VFS tests)
    pkgs.mtools
    pkgs.dosfstools
    pkgs.e2fsprogs        # debugfs for ext2 disk images
    pkgs.e2fsprogs.fuse2fs # fuse2fs for mounting ext2 images without sudo
    pkgs.fuse             # fusermount for fuse2fs
  ];

  shellHook = ''
    # Kernel cross-compilation compiler (unwrapped, no Linux-specific flags)
    export CC="${llvm.clang-unwrapped}/bin/clang"
    export CXX="${llvm.clang-unwrapped}/bin/clang++"

    # Host-target compiler (wrapped Clang+libc++ for native builds)
    export HOST_CC="${hostCC}/bin/cc"
    export HOST_CXX="${hostCC}/bin/c++"

    # Musl cross-compiler for static userspace binaries (DOOM, etc.)
    export MUSL_CC="${muslCC}/bin/x86_64-unknown-linux-musl-gcc"

    # Library paths for host linking
    export LIBRARY_PATH="${llvm.libcxx}/lib:${pkgs.glibc}/lib"
    export LD_LIBRARY_PATH="${llvm.libcxx}/lib"

    export OVMF_CODE="${pkgs.OVMF.fd}/FV/OVMF_CODE.fd"
    export OVMF_VARS="${pkgs.OVMF.fd}/FV/OVMF_VARS.fd"
    export HACK_FONT_TTF="${pkgs.hack-font}/share/fonts/truetype/Hack-Regular.ttf"

    echo "Brook dev shell ready (Clang $(clang --version | head -1))"
  '';
}
