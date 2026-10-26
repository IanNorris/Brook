{ pkgs ? import ../../nix/nixpkgs.nix {} }:
# Keep the upstream package unchanged so its official binary cache is usable.
# Disabling movies here changes the derivation and rebuilds all texture assets.
pkgs.warzone2100
