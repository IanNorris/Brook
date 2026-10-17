{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.vlc.overrideAttrs (old: {
  pname = "vlc-brook";
  patches = (old.patches or []) ++ [
    ./software-format.patch ./memfd.patch ./oss3.patch
    ./wayland-close.patch ./buffer-release.patch ./wayland-window.patch
  ];
  configureFlags = (old.configureFlags or []) ++ [ "--enable-oss" ];
})
