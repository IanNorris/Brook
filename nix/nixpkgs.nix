# Pinned nixpkgs for Brook OS builds.
#
# All tool derivations (tools/*/default.nix) and the development shell
# (shell.nix) import this file instead of <nixpkgs> to ensure every
# builder — regardless of host NixOS channel — produces binaries linked
# against the same glibc, ffmpeg, wayland, etc.
#
# To update: change `rev` and `sha256` below, then rebuild.
# Find the latest commit:  curl -sL https://api.github.com/repos/NixOS/nixpkgs/branches/nixpkgs-unstable | jq .commit.sha
# Prefetch hash:           nix-prefetch-url --unpack https://github.com/NixOS/nixpkgs/archive/<rev>.tar.gz

let
  rev = "68a8af93ff42f2bce34e4f8521a55e5e8a3eb153";  # nixpkgs-unstable 2026-04
  sha256 = ""; # empty = trust-on-first-use (Nix will fetch & cache)
  src = builtins.fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/${rev}.tar.gz";
    # Omitting sha256 allows first use without prefetching. Once cached,
    # the tarball is content-addressed and won't re-download.
  };
in
  import src
