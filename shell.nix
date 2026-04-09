# Brook OS development shell for NixOS / nix-shell
# Usage: nix-shell (from repo root)
#
# Uses unwrapped Clang to avoid the Nix cc-wrapper adding Linux-specific flags
# (e.g. -fPIC) that are invalid for our cross-compilation targets.

{ pkgs ? import <nixpkgs> {} }:

let
  llvm = pkgs.llvmPackages_18;
in
pkgs.mkShell {
  name = "brook-dev";

  buildInputs = [
    # Compiler: unwrapped so cross-targets (x86_64-unknown-windows, x86_64-elf)
    # aren't polluted by the Nix cc-wrapper's Linux-specific flags.
    llvm.clang-unwrapped
    llvm.lld          # provides lld-link and ld.lld

    pkgs.cmake
    pkgs.ninja

    # Runtime / testing
    pkgs.qemu
    pkgs.OVMF

    # Dev tools
    llvm.clang-tools      # clangd, clang-tidy, clang-format

    # Font baking (build-time TTF → kernel glyph atlas)
    pkgs.hack-font
    (pkgs.python3.withPackages (ps: [ ps.freetype-py ]))
  ];

  # Point clang to its own resource dir so freestanding headers are found.
  shellHook = ''
    export CC="${llvm.clang-unwrapped}/bin/clang"
    export CXX="${llvm.clang-unwrapped}/bin/clang++"
    export OVMF_CODE="${pkgs.OVMF.fd}/FV/OVMF_CODE.fd"
    export OVMF_VARS="${pkgs.OVMF.fd}/FV/OVMF_VARS.fd"
    export HACK_FONT_TTF="${pkgs.hack-font}/share/fonts/truetype/Hack-Regular.ttf"
    echo "Brook dev shell ready (Clang $(clang --version | head -1))"
  '';
}
