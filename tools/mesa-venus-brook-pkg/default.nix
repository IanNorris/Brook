# Isolated Venus candidate; do not replace the desktop's Mesa until accepted.
{ pkgs ? import ../../nix/nixpkgs.nix {} }:
(pkgs.mesa.override {
  galliumDrivers = [ "virgl" ];
  vulkanDrivers = [ "virtio" ];
  vulkanLayers = [];
  withValgrind = false;
}).overrideAttrs (old: {
  patches = (old.patches or []) ++ [ ./x11-software-present-wait.patch ];
  # The pin declares this output unconditionally, although the selected
  # drivers do not build the Direct3D shader converter.
  postFixup = (old.postFixup or "") + ''
    mkdir -p "$spirv2dxil"
  '';
})
