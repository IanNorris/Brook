{ pkgs ? import ../../nix/nixpkgs.nix {} }:
# The stock package silently disables Wayland when its scanner is unavailable.
# Require the backend rather than accepting an XCB/KMS-only workload binary.
pkgs.vkmark.overrideAttrs (old: {
  nativeBuildInputs = (old.nativeBuildInputs or []) ++ [ pkgs.wayland-scanner ];
  mesonFlags = (old.mesonFlags or []) ++ [ "-Dwayland=true" ];
  postInstall = (old.postInstall or "") + ''
    test -f "$out/lib/vkmark/wayland.so"
  '';
})
