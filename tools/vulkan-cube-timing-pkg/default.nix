{ pkgs ? import ../../nix/nixpkgs.nix {} }:
# Separate diagnostic package; normal image/launcher packages stay unchanged.
pkgs.vulkan-tools.overrideAttrs (old: {
  pname = "brook-vulkan-cube-timing";
  nativeBuildInputs = (old.nativeBuildInputs or []) ++ [ pkgs.python3 ];
  postPatch = (old.postPatch or "") + ''
    cp ${./cube-timing.h} cube/brook_timing.h
    python3 ${./instrument.py} cube/cube.c
  '';
})
