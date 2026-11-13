# Keep the Iris trial independent of the boot compositor and Vulkan driver.
{ pkgs ? import ../../nix/nixpkgs.nix {}, waylandBufferDepth ? false }:
pkgs.mesa.overrideAttrs (old: {
  patches = (old.patches or []) ++ [ ./batch-completion.patch ]
    ++ pkgs.lib.optional waylandBufferDepth ./source-mesa-wayland-buffer-depth.patch;
})
