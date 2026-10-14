{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.vlc.overrideAttrs (old: {
  pname = "vlc-brook";
  patches = (old.patches or []) ++ [ ./software-format.patch ./memfd.patch ./oss3.patch ];
  configureFlags = (old.configureFlags or []) ++ [ "--enable-oss" ];
})
