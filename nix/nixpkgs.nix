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
  rev = "68a8af93ff42";  # nixpkgs-unstable 2026-04 (26.05pre993588)
  src = builtins.fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/${rev}.tar.gz";
    sha256 = "1slqf8p2178xszw0y1li79kq23kdxnyzdm9ndcrzfwv24fs350d5";
  };
in
  import src
