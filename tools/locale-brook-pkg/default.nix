{ pkgs ? import ../../nix/nixpkgs.nix {} }:
# Small, reproducible UTF-8 archive for glibc apps that require en_US.UTF-8.
# Keep this matched to the pinned glibc; do not copy a host locale archive.
pkgs.glibcLocales.override {
  allLocales = false;
  locales = [ "en_US.UTF-8/UTF-8" ];
}
