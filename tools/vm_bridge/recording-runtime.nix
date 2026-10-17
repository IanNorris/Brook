# Host-only encoding and preview-export runtime; guest disks are unchanged.
{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.buildEnv {
  name = "brook-recording-runtime";
  paths = [
    (pkgs.python3.withPackages (ps: [ ps.av ]))
    pkgs.ffmpeg-headless
  ];
}
