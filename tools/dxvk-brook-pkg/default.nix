{ pkgs ? import ../../nix/nixpkgs.nix {} }:
# Retain the pinned Windows x64 toolchain and Vulkan requirements. SDL3's
# D3D11 renderer requests identity rotation; accept that no-op and report it,
# while leaving real swapchain rotation explicitly unsupported.
pkgs.dxvk.dxvk64.overrideAttrs (old: {
  patches = (old.patches or []) ++ [ ./identity-rotation.patch ];
})
