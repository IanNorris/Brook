# Isolated HasVK experiment; do not replace the desktop Mesa package.
{ pkgs ? import ../../nix/nixpkgs.nix {} }:
(pkgs.mesa.override {
  galliumDrivers = [ "virgl" ];
  vulkanDrivers = [ "intel_hasvk" ];
  vulkanLayers = [];
  withValgrind = false;
}).overrideAttrs (old: {
  patches = (old.patches or []) ++ [ ./optional-capture.patch ];
  postFixup = (old.postFixup or "") + ''
    mkdir -p "$spirv2dxil"
  '';
})
