{ pkgs ? import ../../nix/nixpkgs.nix {} }:
# Keep Wine 11.0 and its existing dependencies; backport the upstream defensive
# check for hosts where interface enumeration fails (including absent netlink).
pkgs.wine64.overrideAttrs (old: {
  patches = (old.patches or []) ++ [ ./nsiproxy-if-nameindex-null.patch ];
})
