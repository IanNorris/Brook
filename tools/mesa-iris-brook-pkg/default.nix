# Keep the Iris trial independent of the boot compositor and Vulkan driver.
{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.mesa.overrideAttrs (old: {
  patches = (old.patches or []) ++ [ ./batch-completion.patch ];
})
